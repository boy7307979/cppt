#pragma once
#include <memory>
#include <vector>
#include <sophus/se3.hpp>
#include "util_datatype.h"
#include "mappoint.h"

class MapPoint;
SMART_PTR(MapPoint)

class Frame : public std::enable_shared_from_this<Frame> {
public:
    Frame(const cv::Mat& img_l, const cv::Mat& img_r, double timestamp);
    ~Frame();

    bool CheckKeyFrame();
    void SetToKeyFrame();

    static uint64_t gNextFrameID, gNextKeyFrameID;
    uint64_t mFrameID;
    bool mIsKeyFrame;
    uint64_t mKeyFrameID;

    Sophus::SE3d mTwc;

    // image points
    std::vector<uint64_t> mvPtID; // this is only for isam2 backend will remove in future
    std::vector<uint32_t> mvPtCount;
    std::vector<cv::Point2f> mv_uv;
    std::vector<cv::Point2f> mvLastKFuv;
    std::vector<float> mv_ur; // value -1 is mono point
    std::vector<MapPointPtr> mvMapPoint; // FIXME
    uint32_t mNumStereo;

    // image
    cv::Mat mImgL, mImgR;
    // image pyr for optical flow
    double mTimeStamp;
    // or CLAHE img?
};

SMART_PTR(Frame)
