#include "line_tracker.h"

LineTracker::LineTracker(CameraPtr camera_) : camera(camera_)
{
    lbd = cv::line_descriptor::BinaryDescriptor::createBinaryDescriptor();
    fld = cv::ximgproc::createFastLineDetector(32, 1.414, 50, 30, 3, true);
    lsd = cv::createLineSegmentDetector(cv::LSD_REFINE_STD);
    //lsd = cv::createLineSegmentDetector(cv::LSD_REFINE_NONE);
}

LineTracker::FramePtr LineTracker::InitFrame(const cv::Mat& img, double timestamp) {
    FramePtr frame(new Frame);
    frame->frame_id = next_frame_id++;
    frame->img = img;
    frame->timestamp = timestamp;

    //FLD
    std::vector<cv::Vec4f> v_lines;
    Tracer::TraceBegin("fld");
#if 1
    fld->detect(frame->img, v_lines);
#else
    lsd->detect(frame->img, v_lines);
#endif
    Tracer::TraceEnd();
    // constrain the number of lines
    if(v_lines.size() > 50) {
        std::sort(v_lines.begin(), v_lines.end(), [](const cv::Vec4f& lhs, const cv::Vec4f& rhs) {
            double ldx = lhs[0] - lhs[2];
            double ldy = lhs[1] - lhs[3];
            double rdx = rhs[0] - rhs[2];
            double rdy = rhs[1] - rhs[3];
            return (ldx * ldx + ldy * ldy) > (rdx * rdx + rdy * rdy);
        });
        v_lines.resize(50);
    }

    for(int i = 0, n = v_lines.size(); i < n; ++i) {
        auto& line = v_lines[i];
        cv::line_descriptor::KeyLine kl;
        double octave_scale = 1.0f;
        int octave_idx = 0;
        kl.startPointX = line[0] * octave_scale;
        kl.startPointY = line[1] * octave_scale;
        kl.endPointX = line[2] * octave_scale;
        kl.endPointY = line[3] * octave_scale;

        kl.sPointInOctaveX = line[0];
        kl.sPointInOctaveY = line[1];
        kl.ePointInOctaveX = line[2];
        kl.ePointInOctaveY = line[3];

        double dx = line[2] - line[0];
        double dy = line[3] - line[1];
        kl.lineLength = std::sqrt(dx * dx + dy * dy);
        kl.angle = std::atan2(dy, dx);
        kl.class_id = i;
        kl.octave = octave_idx;
        kl.size = dx * dy;
        kl.pt = cv::Point2f((line[0] + line[2]) * 0.5, (line[1] + line[3]) * 0.5);

        kl.response = kl.lineLength / std::max(frame->img.rows, frame->img.cols);
        cv::LineIterator li(frame->img, cv::Point2f(line[0], line[1]), cv::Point2f(line[2], line[3]));
        kl.numOfPixels = li.count;
        frame->v_lines.emplace_back(kl);
    }


    // compute lbd descriptor
    Tracer::TraceBegin("lbd");
    if(!frame->v_lines.empty())
        lbd->compute(frame->img, frame->v_lines, frame->desc);
    Tracer::TraceEnd();
    return frame;
}

LineTracker::FramePtr LineTracker::InitFirstFrame(const cv::Mat& img, double timestamp) {
    FramePtr frame = InitFrame(img, timestamp);
    for(int i = 0, n = frame->v_lines.size(); i < n; ++i) {
        frame->v_line_id.emplace_back(next_line_id++);
    }
    last_frame = frame;
    return frame;
}

LineTracker::FramePtr LineTracker::Process(const cv::Mat& img, double timestamp) {
    FramePtr frame = InitFrame(img, timestamp);
    if(frame->v_lines.empty()) {
        last_frame = frame;
        return frame;
    }

    if(last_frame->v_lines.empty()) {
        for(int i = 0, n = frame->v_lines.size(); i < n; ++i) {
            frame->v_line_id.emplace_back(next_line_id++);
        }
        return frame;
    }

    std::vector<int> matches21;
    LRConsistencyMatch(frame->desc, last_frame->desc, 0.75, matches21);

    //    for(int i = 0, n = matches21.size(); i < n; ++i) {
    //        if(matches21[i] != -1)
    //            frame->v_line_id.emplace_back(last_frame->v_line_id[matches21[i]]);
    //        else
    //            frame->v_line_id.emplace_back(next_line_id++);
    //    }

    //record tracking line id
    for (int i1 = 0; i1 < matches21.size(); ++i1) {
        if(matches21[i1] == -1){
            frame->v_line_id.emplace_back(next_line_id++);
            continue;
        }
        //overlap conditional
        cv::line_descriptor::KeyLine kl_tmp = last_frame->v_lines[matches21[i1]];
        cv::line_descriptor::KeyLine kr_tmp = frame->v_lines[i1];

        //TODO:: save undistort point for coverter used.
        Eigen::Vector3d undis_kl_ep;
        Eigen::Vector3d undis_kl_sp;
        Eigen::Vector3d undis_kr_ep;
        Eigen::Vector3d undis_kr_sp;

        undis_kl_sp = camera->BackProject(Eigen::Vector2d(kl_tmp.startPointX, kl_tmp.startPointY));
        undis_kl_ep = camera->BackProject(Eigen::Vector2d(kl_tmp.endPointX, kl_tmp.endPointY));
        undis_kr_sp = camera->BackProject(Eigen::Vector2d(kr_tmp.startPointX, kr_tmp.startPointY));
        undis_kr_ep = camera->BackProject(Eigen::Vector2d(kr_tmp.endPointX, kr_tmp.endPointY));
        Eigen::Vector2d spl;
        Eigen::Vector2d epl;
        Eigen::Vector2d spr;
        Eigen::Vector2d epr;
        camera->Project(undis_kl_sp, spl);
        camera->Project(undis_kl_ep, epl);
        camera->Project(undis_kr_sp, spr);
        camera->Project(undis_kr_ep, epr);


        double overlap = util::f2fLineSegmentOverlap(spl, epl, spr, epr);

        //diff of angle
        Eigen::Vector2d Ii = (spl - epl) / (spl - epl).squaredNorm();
        Eigen::Vector2d Ij = (spr - epr) / (spr - epr).squaredNorm();
        double ij_cross = (Eigen::Vector3d(Ii.x(), Ii.y(), 0).cross(Eigen::Vector3d(Ij.x(), Ij.y(), 0))).norm();
        double ij_dot = Ij.dot(Ii);
        double theta = std::atan2(ij_cross, ij_dot) * 180 / M_PI;
        if(overlap > 0.6 && theta < 5){
            frame->v_line_id.emplace_back(last_frame->v_line_id[matches21[i1]]);
        }
        else{
            frame->v_line_id.emplace_back(next_line_id++);
            matches21[i1] = -1;
        }
    }


#if 0
    std::vector<cv::DMatch> dmatches;

    for(int i = 0, n = matches21.size(); i < n; ++i) {
        if(matches21[i] == -1) {
            continue;
        }
        cv::DMatch dmatch;
        dmatch.queryIdx = i;
        dmatch.trainIdx = matches21[i];
        dmatch.distance = 0;
        dmatch.imgIdx = 0;
        dmatches.emplace_back(dmatch);
    }

    cv::Mat result;
    cv::Mat show_img_j, show_img_i;
    cv::cvtColor(frame->img, show_img_j, CV_GRAY2BGR);
    cv::cvtColor(last_frame->img, show_img_i, CV_GRAY2BGR);
    std::vector<char> mask(dmatches.size(), 1);
    cv::line_descriptor::drawLineMatches(show_img_i, frame->v_lines, show_img_j, last_frame->v_lines,
                                         dmatches, result,
                                         cv::Scalar::all(-1), cv::Scalar::all(-1), mask,
                                         cv::line_descriptor::DrawLinesMatchesFlags::DEFAULT);
    cv::imshow("result", result);
    cv::waitKey(1);
#endif

    last_frame = frame;
    return frame;
}

int LineTracker::Match(const cv::Mat& desc1, const cv::Mat& desc2, float nnr, std::vector<int>& matches12) {
    int num_matches = 0;
    matches12.resize(desc1.rows, -1);

    std::vector<std::vector<cv::DMatch>> dmatches;
    cv::Ptr<cv::BFMatcher> matcher = cv::BFMatcher::create(cv::NORM_HAMMING);
    matcher->knnMatch(desc1, desc2, dmatches, 2);

    for(int i = 0, n = matches12.size(); i < n; ++i) {
        if(dmatches[i][0].distance < dmatches[i][1].distance * nnr) {
            matches12[i] = dmatches[i][0].trainIdx;
            ++num_matches;
        }
    }

    return num_matches;
}

int LineTracker::LRConsistencyMatch(const cv::Mat& desc1, const cv::Mat& desc2, float nnr, std::vector<int>& matches12) {
    std::vector<int> matches21;
    int num_matches = Match(desc1, desc2, nnr, matches12);
    Match(desc2, desc1, nnr, matches21);

    for(int i1 = 0; i1 < matches12.size(); ++i1) {
        int i2 = matches12[i1];
        if(i2 >= 0 && matches21[i2] != i1) {
            matches12[i1] = -1;
            --num_matches;
        }
    }
    return num_matches;
}
