#include "getfeature.h"

// the following code is copied from 2.4.9/modules/python/src2
//
#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include "numpy/ndarrayobject.h"

#include <opencv2/core/core.hpp>

static PyObject* opencv_error = 0;

static int failmsg(const char *fmt, ...)
{
    char str[1000];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(str, sizeof(str), fmt, ap);
    va_end(ap);

    PyErr_SetString(PyExc_TypeError, str);
    return 0;
}

struct ArgInfo
{
    const char *name;
    bool outputarg;
    // more fields may be added if necessary

    ArgInfo(const char * name_, bool outputarg_)
        : name(name_)
        , outputarg(outputarg_) {}

    // to match with older pyopencv_to function signature
    operator const char *() const { return name; }
};

class PyAllowThreads
{
public:
    PyAllowThreads() : _state(PyEval_SaveThread()) {}
    ~PyAllowThreads()
    {
        PyEval_RestoreThread(_state);
    }
private:
    PyThreadState* _state;
};

class PyEnsureGIL
{
public:
    PyEnsureGIL() : _state(PyGILState_Ensure()) {}
    ~PyEnsureGIL()
    {
        PyGILState_Release(_state);
    }
private:
    PyGILState_STATE _state;
};

#define ERRWRAP2(expr) \
try \
{ \
    PyAllowThreads allowThreads; \
    expr; \
} \
catch (const cv::Exception &e) \
{ \
    PyErr_SetString(opencv_error, e.what()); \
    return 0; \
}

using namespace cv;

static PyObject* failmsgp(const char *fmt, ...)
{
  char str[1000];

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(str, sizeof(str), fmt, ap);
  va_end(ap);

  PyErr_SetString(PyExc_TypeError, str);
  return 0;
}
static size_t REFCOUNT_OFFSET = (size_t)&(((PyObject*)0)->ob_refcnt) +
    (0x12345678 != *(const size_t*)"\x78\x56\x34\x12\0\0\0\0\0")*sizeof(int);

static inline PyObject* pyObjectFromRefcount(const int* refcount)
{
    return (PyObject*)((size_t)refcount - REFCOUNT_OFFSET);
}

static inline int* refcountFromPyObject(const PyObject* obj)
{
    return (int*)((size_t)obj + REFCOUNT_OFFSET);
}

class NumpyAllocator : public MatAllocator
{
public:
    NumpyAllocator() {}
    ~NumpyAllocator() {}

    void allocate(int dims, const int* sizes, int type, int*& refcount,
                  uchar*& datastart, uchar*& data, size_t* step)
    {
        PyEnsureGIL gil;

        int depth = CV_MAT_DEPTH(type);
        int cn = CV_MAT_CN(type);
        const int f = (int)(sizeof(size_t)/8);
        int typenum = depth == CV_8U ? NPY_UBYTE : depth == CV_8S ? NPY_BYTE :
                      depth == CV_16U ? NPY_USHORT : depth == CV_16S ? NPY_SHORT :
                      depth == CV_32S ? NPY_INT : depth == CV_32F ? NPY_FLOAT :
                      depth == CV_64F ? NPY_DOUBLE : f*NPY_ULONGLONG + (f^1)*NPY_UINT;
        int i;
        npy_intp _sizes[CV_MAX_DIM+1];
        for( i = 0; i < dims; i++ )
            _sizes[i] = sizes[i];
        if( cn > 1 )
        {
            /*if( _sizes[dims-1] == 1 )
                _sizes[dims-1] = cn;
            else*/
                _sizes[dims++] = cn;
        }
        PyObject* o = PyArray_SimpleNew(dims, _sizes, typenum);
        if(!o)
            CV_Error_(CV_StsError, ("The numpy array of typenum=%d, ndims=%d can not be created", typenum, dims));
        refcount = refcountFromPyObject(o);
        npy_intp* _strides = PyArray_STRIDES((PyArrayObject*) o);
        for( i = 0; i < dims - (cn > 1); i++ )
            step[i] = (size_t)_strides[i];
        datastart = data = (uchar*)PyArray_DATA((PyArrayObject*) o);
    }

    void deallocate(int* refcount, uchar*, uchar*)
    {
        PyEnsureGIL gil;
        if( !refcount )
            return;
        PyObject* o = pyObjectFromRefcount(refcount);
        Py_INCREF(o);
        Py_DECREF(o);
    }
};

NumpyAllocator g_numpyAllocator;

enum { ARG_NONE = 0, ARG_MAT = 1, ARG_SCALAR = 2 };

// special case, when the convertor needs full ArgInfo structure
static int pyopencv_to(const PyObject* o, Mat& m, const ArgInfo info, bool allowND=true)
{
    if(!o || o == Py_None)
    {
        if( !m.data )
            m.allocator = &g_numpyAllocator;
        return true;
    }

    if( PyInt_Check(o) )
    {
        double v[] = {PyInt_AsLong((PyObject*)o), 0., 0., 0.};
        m = Mat(4, 1, CV_64F, v).clone();
        return true;
    }
    if( PyFloat_Check(o) )
    {
        double v[] = {PyFloat_AsDouble((PyObject*)o), 0., 0., 0.};
        m = Mat(4, 1, CV_64F, v).clone();
        return true;
    }
    if( PyTuple_Check(o) )
    {
        int i, sz = (int)PyTuple_Size((PyObject*)o);
        m = Mat(sz, 1, CV_64F);
        for( i = 0; i < sz; i++ )
        {
            PyObject* oi = PyTuple_GET_ITEM(o, i);
            if( PyInt_Check(oi) )
                m.at<double>(i) = (double)PyInt_AsLong(oi);
            else if( PyFloat_Check(oi) )
                m.at<double>(i) = (double)PyFloat_AsDouble(oi);
            else
            {
                failmsg("%s is not a numerical tuple", info.name);
                m.release();
                return false;
            }
        }
        return true;
    }

    if( !PyArray_Check(o) )
    {
        failmsg("%s is not a numpy array, neither a scalar", info.name);
        return false;
    }

    PyArrayObject* oarr = (PyArrayObject*) o;

    bool needcopy = false, needcast = false;
    int typenum = PyArray_TYPE(oarr), new_typenum = typenum;
    int type = typenum == NPY_UBYTE ? CV_8U :
               typenum == NPY_BYTE ? CV_8S :
               typenum == NPY_USHORT ? CV_16U :
               typenum == NPY_SHORT ? CV_16S :
               typenum == NPY_INT ? CV_32S :
               typenum == NPY_INT32 ? CV_32S :
               typenum == NPY_FLOAT ? CV_32F :
               typenum == NPY_DOUBLE ? CV_64F : -1;

    if( type < 0 )
    {
        if( typenum == NPY_INT64 || typenum == NPY_UINT64 || type == NPY_LONG )
        {
            needcopy = needcast = true;
            new_typenum = NPY_INT;
            type = CV_32S;
        }
        else
        {
            failmsg("%s data type = %d is not supported", info.name, typenum);
            return false;
        }
    }

    int ndims = PyArray_NDIM(oarr);
    if(ndims >= CV_MAX_DIM)
    {
        failmsg("%s dimensionality (=%d) is too high", info.name, ndims);
        return false;
    }

    int size[CV_MAX_DIM+1];
    size_t step[CV_MAX_DIM+1], elemsize = CV_ELEM_SIZE1(type);
    const npy_intp* _sizes = PyArray_DIMS(oarr);
    const npy_intp* _strides = PyArray_STRIDES(oarr);
    bool ismultichannel = ndims == 3 && _sizes[2] <= CV_CN_MAX;

    for( int i = ndims-1; i >= 0 && !needcopy; i-- )
    {
        // these checks handle cases of
        //  a) multi-dimensional (ndims > 2) arrays, as well as simpler 1- and 2-dimensional cases
        //  b) transposed arrays, where _strides[] elements go in non-descending order
        //  c) flipped arrays, where some of _strides[] elements are negative
        if( (i == ndims-1 && (size_t)_strides[i] != elemsize) ||
            (i < ndims-1 && _strides[i] < _strides[i+1]) )
            needcopy = true;
    }

    if( ismultichannel && _strides[1] != (npy_intp)elemsize*_sizes[2] )
        needcopy = true;

    if (needcopy)
    {
        if (info.outputarg)
        {
            failmsg("Layout of the output array %s is incompatible with cv::Mat (step[ndims-1] != elemsize or step[1] != elemsize*nchannels)", info.name);
            return false;
        }

        if( needcast ) {
            o = PyArray_Cast(oarr, new_typenum);
            oarr = (PyArrayObject*) o;
        }
        else {
            oarr = PyArray_GETCONTIGUOUS(oarr);
            o = (PyObject*) oarr;
        }

        _strides = PyArray_STRIDES(oarr);
    }

    for(int i = 0; i < ndims; i++)
    {
        size[i] = (int)_sizes[i];
        step[i] = (size_t)_strides[i];
    }

    // handle degenerate case
    if( ndims == 0) {
        size[ndims] = 1;
        step[ndims] = elemsize;
        ndims++;
    }

    if( ismultichannel )
    {
        ndims--;
        type |= CV_MAKETYPE(0, size[2]);
    }

    if( ndims > 2 && !allowND )
    {
        failmsg("%s has more than 2 dimensions", info.name);
        return false;
    }

    m = Mat(ndims, size, type, PyArray_DATA(oarr), step);

    if( m.data )
    {
        m.refcount = refcountFromPyObject(o);
        if (!needcopy)
        {
            m.addref(); // protect the original numpy array from deallocation
                        // (since Mat destructor will decrement the reference counter)
        }
    };
    m.allocator = &g_numpyAllocator;

    return true;
}

static PyObject* pyopencv_from(const Mat& m)
{
    if( !m.data )
        Py_RETURN_NONE;
    Mat temp, *p = (Mat*)&m;
    if(!p->refcount || p->allocator != &g_numpyAllocator)
    {
        temp.allocator = &g_numpyAllocator;
        ERRWRAP2(m.copyTo(temp));
        p = &temp;
    }
    p->addref();
    return pyObjectFromRefcount(p->refcount);
}

// Class GetFeature
//
GetFeature::GetFeature(const std::string &pca_file_path) {
	//初始化pca
    FileStorage fs_pca(pca_file_path,
                       FileStorage::READ);
	fs_pca["mean"] >> pca.mean;
	fs_pca["eigenvalues"] >> pca.eigenvalues;
	fs_pca["eigenvectors"] >> pca.eigenvectors;
	fs_pca.release();
}

/************************************************************************
*功能描述：创建掩膜数组，方便对行人图片每个条带特征提取。
*参数：
*	img：行人图片
*返回：掩膜数组，分别对应从上到下6个水平条带。
**************************************************************************/
Mat* GetFeature::creatMask(const Mat img)const {
	Mat *mask = new Mat[6];

	for (int idx = 0; idx < 6; ++idx) {
		//mask元素初始化为0
		mask[idx] = Mat::zeros(img.rows, img.cols, CV_8UC1);
		//设置ROI
		for (int r = int(img.rows / 6)*idx; r < int(img.rows / 6)*(idx + 1); ++r) {
			for (int c = 0; c < img.cols; ++c) {
				mask[idx].at<uchar>(r, c) = 1;
			}
		}
	}

	//将由于取整操作而未能遍历到的像素点添加到第六个ROI中
	for (int r = int(img.rows / 6) * 6; r < img.rows; ++r) {
		for (int c = 0; c < img.cols; ++c) {
			mask[5].at<uchar>(r, c) = 1;
		}
	}

	return mask;
}

/************************************************************************
*功能描述：获取行人图片的RGB直方图特征，图片被平均分为6个水平条带，分别提取每个水平带各通道的直方图。
*参数：
*	img：行人图片
*	mask：方便提取六个水平条带特征的ROI掩膜
*返回：分别返回每个条带的RGB三个通道的直方图特征，共18个特征向量，每个特征向量维数为16。
**************************************************************************/
Mat* GetFeature::getRGBfeature(const Mat img, Mat* mask)const {
	Mat *feature = new Mat[18];

	//分割成3个单通道图像 ( R, G 和 B )
	Mat* rgb_planes = new Mat[3];
	split(img, rgb_planes);

	/// 设定bin数目 
	int histSize = 16;

	/// 设定取值范围 ( B,G,R)
	float range[] = { 0, 255 };
	const float* histRange = { range };

	bool uniform = true; bool accumulate = false;

	/// 分别计算六个条带RGB各通道直方图:
	for (int idx = 0; idx < 6; ++idx) {
		calcHist(&rgb_planes[0], 1, 0, mask[idx], feature[idx * 3 + 0], 1, &histSize, &histRange, uniform, accumulate);
		calcHist(&rgb_planes[1], 1, 0, mask[idx], feature[idx * 3 + 1], 1, &histSize, &histRange, uniform, accumulate);
		calcHist(&rgb_planes[2], 1, 0, mask[idx], feature[idx * 3 + 2], 1, &histSize, &histRange, uniform, accumulate);
	}

	return feature;
}

/************************************************************************
*功能描述：获取行人图片的HSV直方图特征，图片被平均分为6个水平条带，分别提取每个水平带各通道的直方图。
*参数：
*	img：行人图片
*返回：分别返回每个条带的HSV三个通道的直方图特征，共18个特征向量，每个特征向量维数为16。
**************************************************************************/
Mat* GetFeature::getHSVfeature(const Mat img, Mat* mask)const {
	Mat *feature = new Mat[18];
	Mat hsvImg;

	//转换色彩空间：BGR->HSV
	cvtColor(img, hsvImg, CV_BGR2HSV);

	//分割成3个单通道图像 
	Mat* hsv_planes = new Mat[3];
	split(hsvImg, hsv_planes);

	// 设定bin数目 
	int histSize = 16;

	// 设定取值范围
	float h_ranges[] = { 0, 180 };
	float s_ranges[] = { 0, 255 };
	float v_ranges[] = { 0, 255 };

	const float* histRanges[] = { h_ranges, s_ranges,v_ranges };

	bool uniform = true; bool accumulate = false;

	// 分别计算六个条带HSV各通道直方图:
	for (int idx = 0; idx < 6; ++idx) {
		calcHist(&hsv_planes[0], 1, 0, mask[idx], feature[idx * 3 + 0], 1, &histSize, &histRanges[0], uniform, accumulate);
		calcHist(&hsv_planes[1], 1, 0, mask[idx], feature[idx * 3 + 1], 1, &histSize, &histRanges[1], uniform, accumulate);
		calcHist(&hsv_planes[2], 1, 0, mask[idx], feature[idx * 3 + 2], 1, &histSize, &histRanges[2], uniform, accumulate);
	}

	return feature;
}

/************************************************************************
*功能描述：获取行人图片的YCrCb直方图特征，图片被平均分为6个水平条带，分别提取每个水平带各通道的直方图。
*参数：
*	img：行人图片
*	mask：方便提取六个水平条带特征的ROI掩膜
*返回：分别返回每个条带的YCrCb三个通道的直方图特征，共18个特征向量，每个特征向量维数为16。
**************************************************************************/
Mat* GetFeature::getYCbCrfeature(const Mat img, Mat* mask)const {
	Mat *feature = new Mat[18];
	Mat YCrCbImg;

	//转换色彩空间：BGR->YCrCb
	cvtColor(img, YCrCbImg, CV_BGR2YCrCb);

	//分割成3个单通道图像 
	Mat* YCrCb_planes = new Mat[3];
	split(YCrCbImg, YCrCb_planes);

	// 设定bin数目 
	int histSize = 16;

	// 设定取值范围
	float Y_ranges[] = { 0, 255 };
	float Cr_ranges[] = { 0, 255 };
	float Cb_ranges[] = { 0, 255 };

	const float* histRanges[] = { Y_ranges, Cr_ranges,Cb_ranges };

	bool uniform = true; bool accumulate = false;

	// 分别计算六个条带YCrCb各通道直方图:
	for (int idx = 0; idx < 6; ++idx) {
		calcHist(&YCrCb_planes[0], 1, 0, mask[idx], feature[idx * 3 + 0], 1, &histSize, &histRanges[0], uniform, accumulate);
		calcHist(&YCrCb_planes[1], 1, 0, mask[idx], feature[idx * 3 + 1], 1, &histSize, &histRanges[1], uniform, accumulate);
		calcHist(&YCrCb_planes[2], 1, 0, mask[idx], feature[idx * 3 + 2], 1, &histSize, &histRanges[2], uniform, accumulate);
	}

	return feature;
}

/************************************************************************
*功能描述：获取行人图片的LAB直方图特征，图片被平均分为6个水平条带，分别提取每个水平带各通道的直方图。
*参数：
*	img：行人图片
*	mask：方便提取六个水平条带特征的ROI掩膜
*返回：分别返回每个条带的LAB三个通道的直方图特征，共18个特征向量，每个特征向量维数为16。
**************************************************************************/
Mat* GetFeature::getLabfeature(const Mat img, Mat* mask)const {
	Mat *feature = new Mat[18];
	Mat LabImg;

	//转换色彩空间：BGR->Lab
	cvtColor(img, LabImg, CV_BGR2Lab);

	//分割成3个单通道图像 
	Mat* Lab_planes = new Mat[3];
	split(LabImg, Lab_planes);

	// 设定bin数目 
	int histSize = 16;

	// 设定取值范围
	float L_ranges[] = { 0, 255 };
	float a_ranges[] = { 0, 255 };
	float b_ranges[] = { 0, 255 };

	const float* histRanges[] = { L_ranges, a_ranges,b_ranges };

	bool uniform = true; bool accumulate = false;

	// 分别计算六个条带Lab各通道直方图:
	for (int idx = 0; idx < 6; ++idx) {
		calcHist(&Lab_planes[0], 1, 0, mask[idx], feature[idx * 3 + 0], 1, &histSize, &histRanges[0], uniform, accumulate);
		calcHist(&Lab_planes[1], 1, 0, mask[idx], feature[idx * 3 + 1], 1, &histSize, &histRanges[1], uniform, accumulate);
		calcHist(&Lab_planes[2], 1, 0, mask[idx], feature[idx * 3 + 2], 1, &histSize, &histRanges[2], uniform, accumulate);
	}

	return feature;
}

/************************************************************************
*功能描述：将一维向量src复制到dst的num序号后
*参数：
*	src：源向量
dst：目的向量
num：复制到的位置序号
*返回：无
**************************************************************************/
void GetFeature::cpyMat(Mat src, Mat dst, int& num)const {
	for (int idx = 0; idx < src.rows; idx++) {
		dst.at<float>(num++) = src.at<float>(idx);
	}
}

/************************************************************************
*功能描述：PCA降维
*参数：
*	feature：需要降维的特征矩阵
*返回：PCA降维后结果
**************************************************************************/
Mat GetFeature::doPCA(Mat feature)const {
	return pca.project(feature);
}

/************************************************************************
*功能描述：获取行人图片的特征向量，并进行降维
*参数：
*	img：行人图片
*返回：分别返回每个水平各颜色模型带各通道的直方图特征，6个条带，4种特征，每种特征3个通道，每个通道dims=16,共6*4*3*16=1152维。
**************************************************************************/
Mat GetFeature::getFeature(Mat img) const
{
	Mat *mask = creatMask(img);

	//提取特征
	Mat *rgbFeature = getRGBfeature(img, mask);
	Mat *hsvFeature = getHSVfeature(img, mask);
	Mat *ycbcrFeature = getYCbCrfeature(img, mask);
	Mat *labFeature = getLabfeature(img, mask);

	//创建特征向量
	Mat feature = Mat(1152, 1, CV_32FC1);

	//将提取的直方图特征形成特征向量
	int count = 0;
	for (int stripe = 0; stripe < 6; ++stripe) {
		cpyMat(rgbFeature[stripe * 3 + 0], feature, count);
		cpyMat(rgbFeature[stripe * 3 + 1], feature, count);
		cpyMat(rgbFeature[stripe * 3 + 2], feature, count);
		cpyMat(hsvFeature[stripe * 3 + 0], feature, count);
		cpyMat(hsvFeature[stripe * 3 + 1], feature, count);
		cpyMat(hsvFeature[stripe * 3 + 2], feature, count);
		cpyMat(ycbcrFeature[stripe * 3 + 0], feature, count);
		cpyMat(ycbcrFeature[stripe * 3 + 1], feature, count);
		cpyMat(ycbcrFeature[stripe * 3 + 2], feature, count);
		cpyMat(labFeature[stripe * 3 + 0], feature, count);
		cpyMat(labFeature[stripe * 3 + 1], feature, count);
		cpyMat(labFeature[stripe * 3 + 2], feature, count);
	}
	Mat pcaFeature = doPCA(feature);

	delete[] mask;
	delete[] rgbFeature;
	delete[] hsvFeature;
	delete[] ycbcrFeature;
	delete[] labFeature;

	return pcaFeature;
}

// conveter
//
PyObject *GetFeature::get_feature(PyObject *img) const
{
    cv::Mat cv_image;
    pyopencv_to(img, cv_image, ArgInfo("img", false));

    cv::Mat processed_image = getFeature(cv_image);

    return pyopencv_from(processed_image);
}
