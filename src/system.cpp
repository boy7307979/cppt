﻿#include "system.h"
#include "converter.h"
#include <functional>
#include <glog/logging.h>

System::System(const std::string& config_file) {
    reset_flag = false;
    param = ConfigLoader::Load(config_file);

    if(param.cam_model[0] == "PINHOLE") {
        cam_m = CameraPtr(new Pinhole(param.width[0], param.height[0],
                                      param.intrinsic_master[0][0], param.intrinsic_master[0][1],
                                      param.intrinsic_master[0][2], param.intrinsic_master[0][3],
                                      param.distortion_master[0][0], param.distortion_master[0][1],
                                      param.distortion_master[0][2], param.distortion_master[0][3]));

        cam_s = CameraPtr(new Pinhole(param.width[0], param.height[0],
                                      param.intrinsic_slave[0][0], param.intrinsic_slave[0][1],
                                      param.intrinsic_slave[0][2], param.intrinsic_slave[0][3],
                                      param.distortion_slave[0][0], param.distortion_slave[0][1],
                                      param.distortion_slave[0][2], param.distortion_slave[0][3]));
    }
    else if(param.cam_model[0] == "FISHEYE") {
        cam_m = CameraPtr(new Fisheye(param.width[0], param.height[0],
                                      param.intrinsic_master[0][0], param.intrinsic_master[0][1],
                                      param.intrinsic_master[0][2], param.intrinsic_master[0][3],
                                      param.distortion_master[0][0], param.distortion_master[0][1],
                                      param.distortion_master[0][2], param.distortion_master[0][3]));

        cam_s = CameraPtr(new Fisheye(param.width[0], param.height[0],
                                      param.intrinsic_slave[0][0], param.intrinsic_slave[0][1],
                                      param.intrinsic_slave[0][2], param.intrinsic_slave[0][3],
                                      param.distortion_slave[0][0], param.distortion_slave[0][1],
                                      param.distortion_slave[0][2], param.distortion_slave[0][3]));
    }
    else if(param.cam_model[0] == "OMNI") {
        cam_m = CameraPtr(new Omni(param.width[0], param.height[0],
                                   param.intrinsic_master[0][0], param.intrinsic_master[0][1],
                                   param.intrinsic_master[0][2], param.intrinsic_master[0][3],
                                   param.intrinsic_master[0][4],
                                   param.distortion_master[0][0], param.distortion_master[0][1],
                                   param.distortion_master[0][2], param.distortion_master[0][3]));
        cam_s = CameraPtr(new Omni(param.width[0], param.height[0],
                                   param.intrinsic_slave[0][0], param.intrinsic_slave[0][1],
                                   param.intrinsic_slave[0][2], param.intrinsic_slave[0][3],
                                   param.intrinsic_slave[0][4],
                                   param.distortion_slave[0][0], param.distortion_slave[0][1],
                                   param.distortion_slave[0][2], param.distortion_slave[0][3]));
    }

    feature_tracker = std::make_shared<FeatureTracker>(cam_m);
    stereo_matcher = std::make_shared<StereoMatcher>(cam_m, cam_s, param.p_rl[0], param.q_rl[0]);
    backend = std::make_shared<BackEnd>(cam_m->f(), param.gyr_noise, param.acc_noise,
                                        param.gyr_bias_noise, param.acc_bias_noise,
                                        param.p_rl[0], param.p_bc[0], param.q_rl[0], param.q_bc[0],
                                        param.gravity_magnitude);

    reloc = std::make_shared<Relocalization>(param.voc_filename, param.brief_pattern_file, cam_m,
                                             param.q_bc[0], param.p_bc[0]);
    if(reloc) {
        backend->SubKeyFrame(std::bind(&System::PushKeyFrame2Reloc, this,
                                       std::placeholders::_1,
                                       std::placeholders::_2));
        backend->SubVIOTwc(std::bind(&Relocalization::UpdateVIOPose, reloc,
                                     std::placeholders::_1,
                                     std::placeholders::_2));
    }

    pose_faster = std::make_shared<PoseFaster>(param.q_bc[0], param.p_bc[0], param.gravity_magnitude);
    auto sub_frame = [this](BackEnd::FramePtr frame) {

        if(reloc) {
            Sophus::SE3d Twb = reloc->ShiftPoseWorld(Sophus::SE3d(frame->q_wb, frame->p_wb));
            Eigen::Vector3d v_wb = reloc->ShiftVectorWorld(frame->v_wb);
            pose_faster->UpdatePoseInfo(Twb.translation(), Twb.rotationMatrix(), v_wb,
                                        frame->v_gyr.back(), frame->v_acc.back(), frame->v_imu_timestamp.back(),
                                        frame->ba, frame->bg);
        }
        else {
            pose_faster->UpdatePoseInfo(frame->p_wb, frame->q_wb, frame->v_wb,
                                        frame->v_gyr.back(), frame->v_acc.back(), frame->v_imu_timestamp.back(),
                                        frame->ba, frame->bg);
        }
    };
    backend->SubFrame(sub_frame);
}

System::~System() {}

void System::Reset() {
    reset_flag = true;
    LOG(WARNING) << "system reset flag is turn on!";
}

void System::Process(const cv::Mat& img_l, const cv::Mat& img_r, double timestamp,
                     const Eigen::VecVector3d& v_gyr,
                     const Eigen::VecVector3d& v_acc,
                     const std::vector<double>& v_imu_timestamp)
{
    if(reset_flag) {
        LOG(WARNING) << "reset the system...";
        reset_flag = false;
        b_first_frame = true;
        m_id_history.clear();
        m_id_optical_flow.clear();
        backend->ResetRequest();
    }

    FeatureTracker::FramePtr feat_frame;
    if(b_first_frame) {
        feat_frame = feature_tracker->InitFirstFrame(img_l, timestamp);
        b_first_frame = false;
    }
    else {
        feat_frame = feature_tracker->Process(img_l, timestamp);
    }

    if(!pub_tracking_img.empty()) {
        std::map<uint64_t, cv::Point2f> m_id_history_tmp;
        std::map<uint64_t, std::shared_ptr<std::deque<cv::Point2f>>> m_id_optical_flow_tmp;
        for(int i = 0, n = feat_frame->pt_id.size(); i < n; ++i) {
            auto it = m_id_history.find(feat_frame->pt_id[i]);
            if(it != m_id_history.end()) {
                m_id_history_tmp[it->first] = it->second;
                auto d_pt = m_id_optical_flow[it->first];
                m_id_optical_flow_tmp[it->first] = d_pt;
                d_pt->emplace_back(feat_frame->pt[i]);
                while(d_pt->size() > 5) {
                    d_pt->pop_front();
                }
            }
            else {
                m_id_history_tmp[feat_frame->pt_id[i]] = feat_frame->pt[i];
                m_id_optical_flow_tmp[feat_frame->pt_id[i]] = std::make_shared<std::deque<cv::Point2f>>();
                m_id_optical_flow_tmp[feat_frame->pt_id[i]]->emplace_back(feat_frame->pt[i]);
            }
        }
        m_id_history = std::move(m_id_history_tmp);
        m_id_optical_flow = std::move(m_id_optical_flow_tmp);

        cv::Mat tracking_img = feat_frame->compressed_img.clone();
        for(auto& it : m_id_optical_flow) {
            uint64_t pt_id = it.first;
            std::deque<cv::Point2f>& history_pts = *it.second;
            for(int i = 0, n = history_pts.size() - 1; i < n; ++i) {
                cv::line(tracking_img, history_pts[i] / 2, history_pts[i+1] / 2, cv::Scalar(0, 255, 255), 2);
            }
            cv::circle(tracking_img, history_pts.back() / 2, 2, cv::Scalar(0, 255, 0), -1);
        }

        for(auto& pub : pub_tracking_img) {
            pub(feat_frame->timestamp, tracking_img);
        }
    }

    if(feat_frame->id % 2 == 0) {
        v_cache_gyr.insert(v_cache_gyr.end(), v_gyr.begin(), v_gyr.end());
        v_cache_acc.insert(v_cache_acc.end(), v_acc.begin(), v_acc.end());
        v_cache_imu_timestamps.insert(v_cache_imu_timestamps.end(), v_imu_timestamp.begin(), v_imu_timestamp.end());
        StereoMatcher::FramePtr stereo_frame = stereo_matcher->Process(feat_frame, img_r);
        BackEnd::FramePtr back_frame = Converter::Convert(feat_frame, cam_m, stereo_frame, cam_s,
                                                          v_cache_gyr, v_cache_acc, v_cache_imu_timestamps);

        if(reloc) {
            mtx_reloc_cache.lock();
            d_reloc_cache.emplace_back(feat_frame, back_frame);
            mtx_reloc_cache.unlock();
        }

        backend->PushFrame(back_frame);
    }
    else {
        v_cache_gyr = v_gyr;
        v_cache_acc = v_acc;
        v_cache_imu_timestamps = v_imu_timestamp;
    }
}

void System::PushKeyFrame2Reloc(BackEnd::FramePtr back_frame, const Eigen::VecVector3d& v_x3Dc) {
    std::unique_lock<std::mutex> lock(mtx_reloc_cache);
    int idx = 0;
    for(int i = 0, n = d_reloc_cache.size(); i < n; ++i)
        if(d_reloc_cache[i].second == back_frame) {
            idx = i;
            break;
        }

    FeatureTracker::FramePtr feat_frame = d_reloc_cache[idx].first;
    d_reloc_cache.erase(d_reloc_cache.begin(), d_reloc_cache.begin() + idx);
    lock.unlock();

    int count = 0;
    for(auto& x3Dc : v_x3Dc) {
        if(x3Dc(2) != -1.0)
            ++count;
    }

    Relocalization::FramePtr reloc_frame = Converter::Convert(feat_frame, back_frame, v_x3Dc);
    reloc->PushFrame(reloc_frame);
}
