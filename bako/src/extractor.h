//
// This class is mainly for extracting vectors of
// key frames.
//
// This class will do the following:
//  S1: Extract key frames from video stream.
//  S2: Cut fixed photos with person from key frames.
//  S3: Convert photo into vector.
//
// Input: VideoCapture, current frame
//
#ifndef EXTRACTOR_H
#define EXTRACTOR_H

#include <vector>
#include <string>

#include <opencv2/opencv.hpp>
#include <boost/asio.hpp>
#include "redisclient/redissyncclient.h"
#include "RBML/getfeature.h"
#include "gdatatype.h"
#include "memcache.h"

using namespace std;
using namespace cv;

class Extractor {
public:
    Extractor();
    ~Extractor() {}

    void set_frame_refer(const Mat &frame);
    bool is_init() { return is_init_; }

    // new frame filter
    void handler(const IPCamera ip_camera, const string &video_id,
                 const size_t frame_pos,const Mat &frame);

private:
    // id (char[27]): cam_id + video_id + frame_pos + sequence
    string get_id(const string &cam_id,
                  const string &video_id,
                  const size_t &frame_pos,
                  const int sequence);

private:
    Mat frame_refer_;   // frame reference
    bool is_init_;      // default false
    // for imwrite
    string path_;
    string filename_;

    // using to get feature and PCA
    GetFeature get_feature_;

    // to do memcache
    MemCache memcache_;
};

// Judge keyframe by diff frame.
//
bool FrameDiff(const Mat &frame_t1, const Mat &frame_t2);

// Judge keyframe by diff histogram of frames.
//
bool HistDiff(const Mat &frame_t1, const Mat &frame_t2);

// Human detect.
//
vector<Rect> HumanDetect(const Mat &frame);

#endif // EXTRACTOR_H
