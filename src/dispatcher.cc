#include <iostream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include "sugar/sugar.h"
#include "videostreamhandler.h"
#include "gdebug.h"

using namespace cv;
using std::string;
using std::vector;

const int kBufferSize = 80;
const string g_dir_prefix = "/media/reimondo/HDD/Workspace/Projects/gee/";

void Dispatcher();
void MotionDetect(Mat &frame);
void Test();

int main()
{
    Dispatcher();
    // Test();

    exit(0);
}

void Test()
{
#ifndef NOGDEBUG
    cout << IP2HexStr("192.168.3.1") << endl;
#endif
}

// Dispatcher.
//
void Dispatcher()
{
    char video_stream_addr[kBufferSize];
    // sprintf(video_stream_addr, "%s%s", g_dir_prefix, "cam.sdp");
    sprintf(video_stream_addr, "%s%s",
            g_dir_prefix.c_str(), "example/videos/WP_20151002_09_40_51_Pro_lq.mp4");

    VideoStreamHandler(video_stream_addr, "192.168.3.1");
}

// Motion detect.
//
void MotionDetect(Mat &frame)
{
    // // global variables for test
    // Mat g_fg_mask_MOG2;   // fg mask fg mask generated by MOG2 method
    // // create Background Subtractor objects
    // g_pMOG2 = createBackgroundSubtractorMOG2(); // MOG2 approach

    // // update the backgroud model
    // g_pMOG2->apply(frame, g_fg_mask_MOG2);

    // // find the boundary
    // vector<vector<Point> > contours;
    // vector<Vec4i> hierarchy;
    // findContours(g_fg_mask_MOG2.clone(), contours,
    //              hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);
    // for (int i = 0; i < contours.size(); i++) {
    //     if (contourArea(contours[i]) < 500)
    //         continue;

    //     Rect rect(boundingRect(contours[i]));
    //     rectangle(frame, rect, Scalar(0, 255, 0), 2);
    // }

    // // get the frame number and write in on the current frame
    // rectangle(frame, Point(10, 2), Point(100, 20),
    //           Scalar(255, 255, 255), -1);
    // putText(frame, NumberToString<float>(cap.get(CAP_PROP_POS_FRAMES)),
    //         Point(15, 15), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 0));
    // // show the current frame and
    // imshow("Frame", frame);
    // // show the fg masks
    // threshold(g_fg_mask_MOG2, g_fg_mask_MOG2, 25, 255, THRESH_BINARY);
    // imshow("FG Mask MOG 2", g_fg_mask_MOG2);
}
