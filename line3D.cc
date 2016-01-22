#include "line3D.h"

namespace L3DPP
{
    //------------------------------------------------------------------------------
    Line3D::Line3D(const std::string output_folder, const bool load_segments,
                   const unsigned int max_img_width,
                   const unsigned int max_line_segments,
                   const bool neighbors_by_worldpoints,
                   const bool use_GPU)
    {
        // set params
        data_folder_ = output_folder+"/L3D++_data/";
        max_image_width_ = max_img_width;
        max_line_segments_ = max_line_segments;
        load_segments_ = load_segments;
        neighbors_by_worldpoints_ = neighbors_by_worldpoints;
        num_lines_total_ = 0;

        // default
        collinearity_t_ = L3D_DEF_COLLINEARITY_T;
        num_neighbors_ = L3D_DEF_MATCHING_NEIGHBORS;
        min_baseline_ = L3D_DEF_MIN_BASELINE;
        epipolar_overlap_ = L3D_DEF_EPIPOLAR_OVERLAP;
        kNN_ = L3D_DEF_KNN;
        sigma_p_ = L3D_DEF_SCORING_POS_REGULARIZER;
        sigma_a_ = L3D_DEF_SCORING_ANG_REGULARIZER;
        two_sigA_sqr_ = 2.0f*sigma_a_*sigma_a_;
        perform_RDD_ = false;
        use_CERES_ = false;
        max_iter_CERES_ = L3D_DEF_CERES_MAX_ITER;
        visibility_t_ = 3;

        if(sigma_p_ < L3D_EPS)
        {
            // fixed sigma_p in world-coords
            fixed3Dregularizer_ = true;
            sigma_p_ = fabs(sigma_p_);
        }
        else
        {
            // regularizer in pixels (scale unknown)
            fixed3Dregularizer_ = false;
            sigma_p_ = fmax(0.1f,sigma_p_);
        }

#ifdef L3DPP_CUDA
        useGPU_ = use_GPU;
#else
        useGPU_ = false;
#endif //L3DPP_CUDA

        prefix_ = "[L3D++] ";
        prefix_err_ = prefix_+"ERROR: ";
        prefix_wng_ = prefix_+"WARNING: ";

        // create output directory
        boost::filesystem::path dir(data_folder_);
        boost::filesystem::create_directory(dir);

        std::cout << std::endl;
        std::cout << prefix_ << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << prefix_ << "Line3D++ - http://www.icg.tugraz.at/ - AerialVisionGroup" << std::endl;
        std::cout << prefix_ << "(c) 2015, Manuel Hofer" << std::endl;
        std::cout << prefix_ << "published under the GNU General Public License" << std::endl;
        std::cout << prefix_ << "//////////////////////////////////////////////////////////////////////" << std::endl;
    }

    //------------------------------------------------------------------------------
    Line3D::~Line3D()
    {
        // delete views
        std::map<unsigned int,L3DPP::View*>::iterator it = views_.begin();
        for(; it!=views_.end(); ++it)
        {
            delete it->second;
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::undistortImage(const cv::Mat& inImg, cv::Mat& outImg,
                                const Eigen::Vector3d radial_coeffs,
                                const Eigen::Vector2d tangential_coeffs,
                                const Eigen::Matrix3d& K)
    {
        cv::Mat I = cv::Mat_<double>::eye(3,3);
        cv::Mat cvK = cv::Mat_<double>::zeros(3,3);
        cvK.at<double>(0,0) = K(0,0);
        cvK.at<double>(1,1) = K(1,1);
        cvK.at<double>(0,2) = K(0,2);
        cvK.at<double>(1,2) = K(1,2);
        cvK.at<double>(2,2) = 1.0;

        cv::Mat cvDistCoeffs(5,1,CV_64FC1,cv::Scalar(0));
        cvDistCoeffs.at<double>(0) = radial_coeffs.x();
        cvDistCoeffs.at<double>(1) = radial_coeffs.y();
        cvDistCoeffs.at<double>(2) = tangential_coeffs.x();
        cvDistCoeffs.at<double>(3) = tangential_coeffs.y();
        cvDistCoeffs.at<double>(4) = radial_coeffs.z();

        cv::Mat undistort_map_x;
        cv::Mat undistort_map_y;

        cv::initUndistortRectifyMap(cvK,cvDistCoeffs,I,cvK,cv::Size(inImg.cols, inImg.rows),
                                    undistort_map_x.type(), undistort_map_x, undistort_map_y );
        cv::remap(inImg,outImg,undistort_map_x,undistort_map_y,cv::INTER_LINEAR,cv::BORDER_CONSTANT);
    }

    //------------------------------------------------------------------------------
    void Line3D::addImage(const unsigned int camID, cv::Mat& image,
                          const Eigen::Matrix3d K, const Eigen::Matrix3d R,
                          const Eigen::Vector3d t, const float median_depth,
                          std::list<unsigned int>& wps_or_neighbors,
                          std::vector<cv::Vec4f> line_segments)
    {
        // check ID
        view_reserve_mutex_.lock();
        if(views_reserved_.find(camID) != views_reserved_.end())
        {
            display_text_mutex_.lock();
            std::cout << prefix_err_ << "camera ID [" << camID << "] already in use!" << std::endl;
            display_text_mutex_.unlock();

            view_reserve_mutex_.unlock();
            return;
        }
        else
        {
            // reserve
            views_reserved_[camID] = true;
        }

        if(views_reserved_.size() == 1)
        {
            display_text_mutex_.lock();
            std::cout << std::endl << prefix_ << "[1] ADDING IMAGES ================================" << std::endl;
            display_text_mutex_.unlock();
        }
        view_reserve_mutex_.unlock();

        // check worldpoints
        if(wps_or_neighbors.size() == 0)
        {
            display_text_mutex_.lock();
            if(neighbors_by_worldpoints_)
                std::cout << prefix_err_ << "view [" << camID << "] has no worldpoints!" << std::endl;
            else
                std::cout << prefix_err_ << "view [" << camID << "] has no visual neighbors!" << std::endl;

            display_text_mutex_.unlock();

            return;
        }

        // detect segments
        L3DPP::DataArray<float4>* lines = NULL;
        if(line_segments.size() == 0)
        {
            // detect segments using LSD algorithm
            lines = detectLineSegments(camID,image);
        }
        else
        {
            // use given segments
            lines = new L3DPP::DataArray<float4>(line_segments.size(),1);
            for(size_t i=0; i<line_segments.size(); ++i)
            {
                cv::Vec4f coords = line_segments[i];
                float4 coordsf4;
                coordsf4.x = coords(0); coordsf4.y = coords(1);
                coordsf4.z = coords(2); coordsf4.w = coords(3);
                lines->dataCPU(i,0)[0] = coordsf4;
            }
        }

        if(lines == NULL)
        {
            display_text_mutex_.lock();
            std::cout << prefix_wng_ << "no line segments found in image [" << camID << "]!" << std::endl;
            display_text_mutex_.unlock();

            return;
        }

        display_text_mutex_.lock();
        std::cout << prefix_ << "adding view [" << std::setfill('0') << std::setw(L3D_DISP_CAMS) << camID;
        std::cout << "]: #lines = " << std::setfill(' ') << std::setw(L3D_DISP_LINES) << lines->width();
        std::cout << " [" << std::setfill('0') << std::setw(L3D_DISP_CAMS) << views_.size() << "]" << std::endl;
        display_text_mutex_.unlock();

        // create view
        L3DPP::View* v = new L3DPP::View(camID,lines,K,R,t,image.cols,image.rows,median_depth);
        view_mutex_.lock();
        views_[camID] = v;
        view_order_.push_back(camID);
        matches_[camID] = std::vector<std::list<L3DPP::Match> >(lines->width());
        num_matches_[camID] = 0;
        processed_[camID] = false;
        visual_neighbors_[camID] = std::map<unsigned int,bool>();
        num_lines_total_ += lines->width();

        if(neighbors_by_worldpoints_)
        {
            // process worldpoint list
            processWPlist(camID,wps_or_neighbors);
        }
        else
        {
            // neighbors explicitely given
            setVisualNeighbors(camID,wps_or_neighbors);
        }
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::processWPlist(const unsigned int camID, std::list<unsigned int>& wps)
    {
        std::list<unsigned int>::iterator it = wps.begin();
        for(; it!=wps.end(); ++it)
        {
            unsigned int wpID = *it;
            worldpoints2views_[wpID].push_back(camID);
        }
        num_worldpoints_[camID] = wps.size();
        views2worldpoints_[camID] = wps;
    }

    //------------------------------------------------------------------------------
    void Line3D::setVisualNeighbors(const unsigned int camID, std::list<unsigned int>& neighbors)
    {
        fixed_visual_neighbors_[camID] = neighbors;
    }

    //------------------------------------------------------------------------------
    L3DPP::DataArray<float4>* Line3D::detectLineSegments(const unsigned int camID, cv::Mat& image)
    {
        // check image format
        cv::Mat imgGray;
        if(image.type() == CV_8UC3)
        {
            // convert to grayscale
            cv::cvtColor(image,imgGray,CV_RGB2GRAY);
        }
        else if(image.type() == CV_8U)
        {
            imgGray = image.clone();
        }
        else
        {
            display_text_mutex_.lock();
            std::cout << prefix_err_ << "image type not supported! must be CV_8U (gray) or CV_8UC3 (RGB)!" << std::endl;
            display_text_mutex_.unlock();
            return NULL;
        }

        // check image size
        unsigned int max_dim = std::max(imgGray.rows,imgGray.cols);
        float upscale_x = 1.0f;
        float upscale_y = 1.0f;
        unsigned int new_width = imgGray.cols;
        unsigned int new_height = imgGray.rows;

        cv::Mat imgResized;
        if(max_dim > max_image_width_)
        {
            // rescale
            float s = float(max_image_width_)/float(max_dim);
            cv::resize(imgGray,imgResized,cv::Size(),s,s);

            upscale_x = float(imgGray.cols)/float(imgResized.cols);
            upscale_y = float(imgGray.rows)/float(imgResized.rows);

            new_width = imgResized.cols;
            new_height = imgResized.rows;
        }
        else
        {
            imgResized = imgGray.clone();
        }

        // see if lines already exist
        L3DPP::DataArray<float4>* segments = NULL;
        std::stringstream str;
        if(load_segments_)
        {
            str << data_folder_ << "segments_L3D++_" << camID << "_" << new_width << "x" << new_height << ".bin";

            boost::filesystem::path file(str.str());
            if(boost::filesystem::exists(file))
            {
                segments = new L3DPP::DataArray<float4>();
                L3DPP::serializeFromFile(str.str(),*segments);
                return segments;
            }
        }

        // detect line segments
#ifndef L3DPP_OPENCV3
        cv::Ptr<cv::LineSegmentDetector> lsd = cv::createLineSegmentDetectorPtr(cv::LSD_REFINE_ADV);
#else
        cv::Ptr<cv::LineSegmentDetector> lsd = cv::createLineSegmentDetector(cv::LSD_REFINE_ADV);
#endif //L3DPP_LSD_EXT
        std::vector<cv::Vec4f> detections;
        lsd->detect(imgResized,detections);

        float diag = sqrtf(float(image.rows*image.rows)+float(image.cols*image.cols));
        float min_len = diag*L3D_DEF_MIN_LINE_LENGTH_FACTOR;

        L3DPP::lines2D_sorted_by_length sorted;
        for(size_t i=0; i<detections.size(); ++i)
        {
            cv::Vec4f data = detections[i];

            L3DPP::SegmentData2D seg2D;
            seg2D.p1x_ = data(0)*upscale_x;
            seg2D.p1y_ = data(1)*upscale_y;
            seg2D.p2x_ = data(2)*upscale_x;
            seg2D.p2y_ = data(3)*upscale_y;

            float dx = seg2D.p1x_-seg2D.p2x_;
            float dy = seg2D.p1y_-seg2D.p2y_;
            seg2D.length_ = sqrtf(dx*dx + dy*dy);

            if(seg2D.length_ > min_len)
                sorted.push(seg2D);
        }

        if(sorted.size() > 0)
        {
            // convert to dataArray
            if(sorted.size() < max_line_segments_)
                segments = new L3DPP::DataArray<float4>(sorted.size(),1);
            else
                segments = new L3DPP::DataArray<float4>(max_line_segments_,1);

            unsigned int pos = 0;
            while(!sorted.empty() && pos < segments->width())
            {
                L3DPP::SegmentData2D segData = sorted.top();
                float4 coordsf4;
                coordsf4.x = segData.p1x_; coordsf4.y = segData.p1y_;
                coordsf4.z = segData.p2x_; coordsf4.w = segData.p2y_;
                segments->dataCPU(pos,0)[0] = coordsf4;
                sorted.pop();
                ++pos;
            }

            // save
            if(load_segments_)
            {
                L3DPP::serializeToFile(str.str(),*segments);
            }

            return segments;
        }

        return NULL;
    }

    //------------------------------------------------------------------------------
    void Line3D::matchImages(const float sigma_position, const float sigma_angle,
                             const unsigned int num_neighbors, const float epipolar_overlap,
                             const float min_baseline, const int kNN)
    {
        // new views can be added in the meantime!
        view_reserve_mutex_.lock();
        view_mutex_.lock();

        std::cout << std::endl << prefix_ << "[2] LINE MATCHING ================================" << std::endl;

        if(views_.size() == 0)
        {
            std::cout << prefix_wng_ << "no images to match! forgot to add them?" << std::endl;
            view_mutex_.unlock();
            view_reserve_mutex_.unlock();
            return;
        }

        // check params
        num_neighbors_ = std::max(int(num_neighbors),2);
        sigma_p_ = sigma_position;
        sigma_a_ = fmin(fabs(sigma_angle),90.0f);
        two_sigA_sqr_ = 2.0f*sigma_a_*sigma_a_;
        min_baseline_ = fmax(min_baseline,0.0f);
        epipolar_overlap_ = fmin(fabs(epipolar_overlap),0.99f);
        kNN_ = kNN;

        if(sigma_p_ < 0.0f)
        {
            // fixed sigma_p in world-coords
            fixed3Dregularizer_ = true;
            sigma_p_ = fabs(sigma_p_);
        }
        else
        {
            // regularizer in pixels (scale unknown)
            fixed3Dregularizer_ = false;
            sigma_p_ = fmax(0.1f,sigma_p_);
        }

        // reset
        matched_.clear();
        estimated_position3D_.clear();
        entry_map_.clear();

        // compute spatial regularizer
        if(!fixed3Dregularizer_)
            std::cout << prefix_ << "computing spatial regularizers... [" << sigma_p_ << " px]" << std::endl;
        else
            std::cout << prefix_ << "computing spatial regularizers... [" << sigma_p_ << " m]" << std::endl;

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<view_order_.size(); ++i)
        {
            unsigned int camID = view_order_[i];

            if(!fixed3Dregularizer_)
                views_[camID]->computeSpatialRegularizer(sigma_p_);
            else
                views_[camID]->update_k(sigma_p_);

            // reset matches
            matches_[camID] = std::vector<std::list<L3DPP::Match> >(views_[camID]->num_lines());
            num_matches_[camID] = 0;
            processed_[camID] = false;
        }

        // find visual neighbors
        std::cout << prefix_ << "computing visual neighbors...     [" << num_neighbors_ << " imgs.]" << std::endl;

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<view_order_.size(); ++i)
        {
            unsigned int camID = view_order_[i];

            if(fixed_visual_neighbors_.find(camID) != fixed_visual_neighbors_.end())
            {
                if(visual_neighbors_[camID].size() == 0)
                {
                    // fixed neighbors
                    std::list<unsigned int>::iterator n_it = fixed_visual_neighbors_[camID].begin();
                    for(; n_it!=fixed_visual_neighbors_[camID].end(); ++n_it)
                    {
                        if(views_.find(*n_it) != views_.end())
                            visual_neighbors_[camID][*n_it] = true;
                    }
                }
            }
            else
            {
                // compute neighbors from WP overlap
                findVisualNeighborsFromWPs(camID);
            }
        }

        // match images
        std::cout << prefix_ << "computing matches..." << std::endl;

        computeMatches();

        view_mutex_.unlock();
        view_reserve_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::findVisualNeighborsFromWPs(const unsigned int camID)
    {
        if(visual_neighbors_.find(camID) != visual_neighbors_.end())
        {
            // reset
            visual_neighbors_[camID].clear();
            std::map<unsigned int,unsigned int> commonWPs;

            std::list<unsigned int>::iterator wp_it = views2worldpoints_[camID].begin();
            for(; wp_it!=views2worldpoints_[camID].end(); ++wp_it)
            {
                // iterate over worldpoints
                unsigned int wpID = *wp_it;

                std::list<unsigned int>::iterator view_it = worldpoints2views_[wpID].begin();
                for(; view_it!=worldpoints2views_[wpID].end(); ++view_it)
                {
                    // all views are potential neighbors
                    unsigned int vID = *view_it;
                    if(vID != camID)
                    {
                        if(commonWPs.find(vID) == commonWPs.end())
                        {
                            commonWPs[vID] = 1;
                        }
                        else
                        {
                            ++commonWPs[vID];
                        }
                    }
                }
            }

            if(commonWPs.size() == 0)
                return;

            // find visual neighbors
            std::map<unsigned int,bool> used_neighbors;
            std::list<L3DPP::VisualNeighbor> neighbors;
            L3DPP::View* v = views_[camID];
            std::map<unsigned int,unsigned int>::iterator c_it = commonWPs.begin();
            for(; c_it!=commonWPs.end(); ++c_it)
            {
                unsigned int vID = c_it->first;
                unsigned int num_common_wps = c_it->second;

                VisualNeighbor vn;
                vn.camID_ = vID;
                vn.score_ = 2.0f*float(num_common_wps)/float(num_worldpoints_[camID]+num_worldpoints_[vID]);
                vn.axisAngle_ = v->opticalAxesAngle(views_[vID]);

                // check baseline
                if(vn.axisAngle_ < 1.571f) // ~ PI/2
                {
                    neighbors.push_back(vn);
                }
            }

            // sort by score
            neighbors.sort(L3DPP::sortVisualNeighborsByScore);

            /*
            std::list<L3DPP::VisualNeighbor> wide_baseline = neighbors;
            if(wide_baseline.size() > 2*num_neighbors_)
                wide_baseline.resize(2*num_neighbors_);

            // sort by angle
            wide_baseline.sort(L3DPP::sortVisualNeighborsByAngle);

            // wide baseline neighbors -> store in visual neighbor map
            std::list<L3DPP::VisualNeighbor>::iterator nit = wide_baseline.begin();
            while(nit!=wide_baseline.end() && used_neighbors.size() < num_neighbors_/2)
            {
                L3DPP::VisualNeighbor vn = *nit;
                L3DPP::View* v2 = views_[vn.camID_];

                // check baseline
                if(v->baseLine(v2) > min_baseline_)
                {
                    std::map<unsigned int,bool>::iterator u_it = used_neighbors.begin();
                    bool valid = true;
                    for(; u_it!=used_neighbors.end() && valid; ++u_it)
                    {
                        if(!(v->baseLine(views_[u_it->first]) > min_baseline_))
                            valid = false;
                    }

                    if(valid)
                        used_neighbors[vn.camID_] = true;
                }

                ++nit;
            }
            */

            // highscore neighbors -> store in visual neighbor map
            std::list<L3DPP::VisualNeighbor>::iterator nit = neighbors.begin();
            while(nit!=neighbors.end() && used_neighbors.size() < num_neighbors_)
            {
                L3DPP::VisualNeighbor vn = *nit;
                L3DPP::View* v2 = views_[vn.camID_];

                // check baseline
                if(used_neighbors.find(vn.camID_) == used_neighbors.end() && v->baseLine(v2) > min_baseline_)
                {
                    std::map<unsigned int,bool>::iterator u_it = used_neighbors.begin();
                    bool valid = true;
                    for(; u_it!=used_neighbors.end() && valid; ++u_it)
                    {
                        if(!(v->baseLine(views_[u_it->first]) > min_baseline_))
                            valid = false;
                    }

                    if(valid)
                        used_neighbors[vn.camID_] = true;
                }

                ++nit;
            }

            visual_neighbors_[camID] = used_neighbors;
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::computeMatches()
    {
        std::map<unsigned int,std::map<unsigned int,bool> >::iterator it = visual_neighbors_.begin();
        for(; it!=visual_neighbors_.end(); ++it)
        {
            std::cout << prefix_;
            if(useGPU_)
                std::cout << "@GPU: ";
            else
                std::cout << "@CPU: ";

            std::cout << "[" << std::setfill('0') << std::setw(L3D_DISP_CAMS) << it->first << "] --> ";

            // init GPU data
            if(useGPU_)
                initSrcDataGPU(it->first);

            std::map<unsigned int,bool>::iterator n_it = it->second.begin();
            for(; n_it!=it->second.end(); ++n_it)
            {
                if(matched_[it->first].find(n_it->first) == matched_[it->first].end())
                {
                    // not yet matched
                    std::cout << "[" << std::setfill('0') << std::setw(L3D_DISP_CAMS) << n_it->first << "] ";

                    // compute fundamental matrix
                    Eigen::Matrix3d F = getFundamentalMatrix(views_[it->first],
                                                             views_[n_it->first]);

                    // matching
                    if(useGPU_)
                        matchingGPU(it->first,n_it->first,F);
                    else
                        matchingCPU(it->first,n_it->first,F);

                    // set matched
                    matched_[it->first][n_it->first] = true;
                    matched_[n_it->first][it->first] = true;
                }
            }

            std::cout << "done!" << std::endl;

            // scoring
            float valid_f;

            if(useGPU_)
                scoringGPU(it->first,valid_f);
            else
                scoringCPU(it->first,valid_f);

            std::cout << prefix_ << "scoring: " << "clusterable_segments=" << int(valid_f*100) << "%";
            std::cout << std::endl;

            // cleanup GPU data
            if(useGPU_)
                removeSrcDataGPU(it->first);

            // store inverse matches
            storeInverseMatches(it->first);

            // filter invalid matches
            filterMatches(it->first);

            // set processed
            processed_[it->first] = true;

            std::cout << prefix_ << "#matches: ";
            std::cout << std::setfill(' ') << std::setw(L3D_DISP_MATCHES) << num_matches_[it->first] << std::endl;
            std::cout << prefix_ << "median_depth: " << views_[it->first]->median_depth() << std::endl;
        }
    }

    //------------------------------------------------------------------------------
    Eigen::Matrix3d Line3D::getFundamentalMatrix(L3DPP::View* src, L3DPP::View* tgt)
    {
        // check if it already exists
        if(fundamentals_[src->id()].find(tgt->id()) != fundamentals_[src->id()].end())
        {
            return fundamentals_[src->id()][tgt->id()];
        }
        else if(fundamentals_[tgt->id()].find(src->id()) != fundamentals_[tgt->id()].end())
        {
            Eigen::Matrix3d Ft = fundamentals_[tgt->id()][src->id()].transpose();
            return Ft;
        }

        // compute new fundamental matrix
        Eigen::Matrix3d K1 = src->K();
        Eigen::Matrix3d R1 = src->R();
        Eigen::Vector3d t1 = src->t();

        Eigen::Matrix3d K2 = tgt->K();
        Eigen::Matrix3d R2 = tgt->R();
        Eigen::Vector3d t2 = tgt->t();

        Eigen::Matrix3d R = R2 * R1.transpose();
        Eigen::Vector3d t = t2 - R * t1;

        Eigen::Matrix3d T(3,3);
        T(0,0) = 0.0;    T(0,1) = -t.z(); T(0,2) = t.y();
        T(1,0) = t.z();  T(1,1) = 0.0;    T(1,2) = -t.x();
        T(2,0) = -t.y(); T(2,1) = t.x();  T(2,2) = 0.0;

        Eigen::Matrix3d E = T * R;
        Eigen::Matrix3d F = K2.transpose().inverse() * E * K1.inverse();

        fundamentals_[src->id()][tgt->id()] = F;

        return F;
    }

    //------------------------------------------------------------------------------
    void Line3D::matchingCPU(const unsigned int src, const unsigned int tgt,
                             Eigen::Matrix3d& F)
    {
        L3DPP::View* v_src = views_[src];
        L3DPP::View* v_tgt = views_[tgt];

        L3DPP::DataArray<float4>* lines_src = v_src->lines();
        L3DPP::DataArray<float4>* lines_tgt = v_tgt->lines();

        unsigned int num_matches = 0;

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t r=0; r<lines_src->width(); ++r)
        {
            int new_matches = 0;

            // source line
            Eigen::Vector3d p1(lines_src->dataCPU(r,0)[0].x,
                               lines_src->dataCPU(r,0)[0].y,1.0);
            Eigen::Vector3d p2(lines_src->dataCPU(r,0)[0].z,
                               lines_src->dataCPU(r,0)[0].w,1.0);

            // epipolar lines
            Eigen::Vector3d epi_p1 = F*p1;
            Eigen::Vector3d epi_p2 = F*p2;

            // use priority queue when kNN > 0
            L3DPP::pairwise_matches scored_matches;

            for(size_t c=0; c<lines_tgt->width(); ++c)
            {
                // target line
                Eigen::Vector3d q1(lines_tgt->dataCPU(c,0)[0].x,
                                   lines_tgt->dataCPU(c,0)[0].y,1.0);
                Eigen::Vector3d q2(lines_tgt->dataCPU(c,0)[0].z,
                                   lines_tgt->dataCPU(c,0)[0].w,1.0);
                Eigen::Vector3d l2 = q1.cross(q2);

                // intersect
                Eigen::Vector3d p1_proj = l2.cross(epi_p1);
                Eigen::Vector3d p2_proj = l2.cross(epi_p2);

                if(fabs(p1_proj.z()) > L3D_EPS && fabs(p2_proj.z()) > L3D_EPS)
                {
                    // normalize
                    p1_proj /= p1_proj.z();
                    p2_proj /= p2_proj.z();

                    // check overlap
                    std::vector<Eigen::Vector3d> collinear_points(4);
                    collinear_points[0] = p1_proj;
                    collinear_points[1] = p2_proj;
                    collinear_points[2] = q1;
                    collinear_points[3] = q2;
                    float score = mutualOverlap(collinear_points);

                    if(score > epipolar_overlap_)
                    {
                        // triangulate
                        Eigen::Vector2d depths_src = triangulationDepths(src,p1,p2,
                                                                         tgt,q1,q2);
                        Eigen::Vector2d depths_tgt = triangulationDepths(tgt,q1,q2,
                                                                         src,p1,p2);

                        if(depths_src.x() > L3D_EPS && depths_src.y() > L3D_EPS &&
                                depths_tgt.x() > L3D_EPS && depths_tgt.y() > L3D_EPS)
                        {
                            // potential match
                            L3DPP::Match M;
                            M.src_camID_ = src;
                            M.src_segID_ = r;
                            M.tgt_camID_ = tgt;
                            M.tgt_segID_ = c;
                            M.overlap_score_ = score;
                            M.score3D_ = 0.0f;
                            M.depth_p1_ = depths_src.x();
                            M.depth_p2_ = depths_src.y();
                            M.depth_q1_ = depths_tgt.x();
                            M.depth_q2_ = depths_tgt.y();

                            if(kNN_ > 0)
                            {
                                // kNN matching
                                scored_matches.push(M);
                            }
                            else
                            {
                                // all matches are used
                                matches_[src][r].push_back(M);
                                ++new_matches;
                            }
                        }
                    }
                }
            }

            // push kNN matches into list
            if(kNN_ > 0)
            {
                while(new_matches < kNN_ && !scored_matches.empty())
                {
                    matches_[src][r].push_back(scored_matches.top());
                    scored_matches.pop();
                    ++new_matches;
                }
            }

            match_mutex_.lock();
            num_matches += new_matches;
            match_mutex_.unlock();
        }

        num_matches_[src] += num_matches;
    }

    //------------------------------------------------------------------------------
    void Line3D::initSrcDataGPU(const unsigned int src)
    {
#ifdef L3DPP_CUDA
        // upload
        L3DPP::View* v1 = views_[src];
        v1->lines()->upload();
        v1->RtKinvGPU()->upload();
#endif //L3DPP_CUDA
    }

    //------------------------------------------------------------------------------
    void Line3D::removeSrcDataGPU(const unsigned int src)
    {
#ifdef L3DPP_CUDA
        // cleanup
        L3DPP::View* v1 = views_[src];
        v1->lines()->removeFromGPU();
        v1->RtKinvGPU()->removeFromGPU();
#endif //L3DPP_CUDA
    }

    //------------------------------------------------------------------------------
    void Line3D::matchingGPU(const unsigned int src, const unsigned int tgt,
                             Eigen::Matrix3d& F)
    {
#ifdef L3DPP_CUDA
        // INFO: src data must be on GPU! initSrcDataGPU(src)
        L3DPP::View* v1 = views_[src];

        // upload segments to GPU
        L3DPP::View* v2 = views_[tgt];
        v2->lines()->upload();

        // move F to GPU
        L3DPP::DataArray<float>* F_GPU = NULL;
        eigen2dataArray(F_GPU,F);
        F_GPU->upload();

        // move RtKinv to GPU
        v2->RtKinvGPU()->upload();

        // match segments on GPU
        unsigned int num_matches = L3DPP::match_lines_GPU(v1->lines(),v2->lines(),F_GPU,
                                                          v1->RtKinvGPU(),v2->RtKinvGPU(),
                                                          v1->C_GPU(),v2->C_GPU(),
                                                          &(matches_[src]),src,tgt,
                                                          epipolar_overlap_,kNN_);

        num_matches_[src] += num_matches;

        // cleanup
        v2->lines()->removeFromGPU();
        v2->RtKinvGPU()->removeFromGPU();
        delete F_GPU;

#endif //L3DPP_CUDA
    }

    //------------------------------------------------------------------------------
    bool Line3D::pointOnSegment(const Eigen::Vector3d x, const Eigen::Vector3d p1,
                                const Eigen::Vector3d p2)
    {
        Eigen::Vector2d v1(p1.x()-x.x(),p1.y()-x.y());
        Eigen::Vector2d v2(p2.x()-x.x(),p2.y()-x.y());
        return (v1.dot(v2) < L3D_EPS);
    }

    //------------------------------------------------------------------------------
    float Line3D::mutualOverlap(std::vector<Eigen::Vector3d>& collinear_points)
    {
        float overlap = 0.0f;

        if(collinear_points.size() != 4)
            return 0.0f;

        Eigen::Vector3d p1 = collinear_points[0];
        Eigen::Vector3d p2 = collinear_points[1];
        Eigen::Vector3d q1 = collinear_points[2];
        Eigen::Vector3d q2 = collinear_points[3];

        if(pointOnSegment(p1,q1,q2) || pointOnSegment(p2,q1,q2) ||
                pointOnSegment(q1,p1,p2) || pointOnSegment(q2,p1,p2))
        {
            // find outer distance and inner points
            float max_dist = 0.0f;
            size_t outer1 = 0;
            size_t inner1 = 1;
            size_t inner2 = 2;
            size_t outer2 = 3;

            for(size_t i=0; i<3; ++i)
            {
                for(size_t j=i+1; j<4; ++j)
                {
                    float dist = (collinear_points[i]-collinear_points[j]).norm();
                    if(dist > max_dist)
                    {
                        max_dist = dist;
                        outer1 = i;
                        outer2 = j;
                    }
                }
            }

            if(max_dist < 1.0f)
                return 0.0f;

            if(outer1 == 0)
            {
                if(outer2 == 1)
                {
                    inner1 = 2;
                    inner2 = 3;
                }
                else if(outer2 == 2)
                {
                    inner1 = 1;
                    inner2 = 3;
                }
                else
                {
                    inner1 = 1;
                    inner2 = 2;
                }
            }
            else if(outer1 == 1)
            {
                inner1 = 0;
                if(outer2 == 2)
                {
                    inner2 = 3;
                }
                else
                {
                    inner2 = 2;
                }
            }
            else
            {
                inner1 = 0;
                inner2 = 1;
            }

            overlap = (collinear_points[inner1]-collinear_points[inner2]).norm()/max_dist;

        }
        return overlap;
    }

    //------------------------------------------------------------------------------
    Eigen::Vector2d Line3D::triangulationDepths(const unsigned int src_camID, const Eigen::Vector3d p1,
                                                const Eigen::Vector3d p2, const unsigned int tgt_camID,
                                                const Eigen::Vector3d line_q1, const Eigen::Vector3d line_q2)
    {
        L3DPP::View* v_src = views_[src_camID];
        L3DPP::View* v_tgt = views_[tgt_camID];

        // rays through points
        Eigen::Vector3d C1 = v_src->C();
        Eigen::Vector3d ray_p1 = v_src->getNormalizedRay(p1);
        Eigen::Vector3d ray_p2 = v_src->getNormalizedRay(p2);

        // plane
        Eigen::Vector3d C2 = v_tgt->C();
        Eigen::Vector3d ray_q1 = v_tgt->getNormalizedRay(line_q1);
        Eigen::Vector3d ray_q2 = v_tgt->getNormalizedRay(line_q2);
        Eigen::Vector3d n = ray_q1.cross(ray_q2);
        n.normalize();

        if(fabs(ray_p1.dot(n)) < L3D_EPS || fabs(ray_p2.dot(n)) < L3D_EPS)
            return Eigen::Vector2d(-1,-1);

        double d1 = (C2.dot(n) - n.dot(C1)) / (n.dot(ray_p1));
        double d2 = (C2.dot(n) - n.dot(C1)) / (n.dot(ray_p2));
        return Eigen::Vector2d(d1,d2);
    }

    //------------------------------------------------------------------------------
    void Line3D::sortMatches(const unsigned int src)
    {
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            matches_[src][i].sort(L3DPP::sortMatchesByIDs);
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::scoringCPU(const unsigned int src, float& valid_f)
    {
        // init
        valid_f = 0.0f;
        L3DPP::View* v = views_[src];
        float k = v->k();

        unsigned int num_valid = 0;

        // iterative scoring
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            std::map<unsigned int,bool> valid_matches_exist;

            std::list<L3DPP::Match>::iterator it = matches_[src][i].begin();
            for(; it!=matches_[src][i].end(); ++it)
            {
                L3DPP::Match M = *it;
                float score3D = 0.0f;
                std::map<unsigned int,float> score_per_cam;

                std::list<L3DPP::Match>::iterator it2 = matches_[src][i].begin();
                for(; it2!=matches_[src][i].end(); ++it2)
                {
                    L3DPP::Match M2 = *it2;

                    if(M.tgt_camID_ != M2.tgt_camID_)
                    {
                        // compute similarity
                        float sim = similarityForScoring(M,M2,k);

                        if(score_per_cam.find(M2.tgt_camID_) != score_per_cam.end())
                        {
                            if(sim > score_per_cam[M2.tgt_camID_])
                            {
                                score3D -= score_per_cam[M2.tgt_camID_];
                                score3D += sim;
                                score_per_cam[M2.tgt_camID_] = sim;
                            }
                        }
                        else
                        {
                            score3D += sim;
                            score_per_cam[M2.tgt_camID_] = sim;
                        }
                    }
                }

                (*it).score3D_ = score3D;
                if(score3D > L3D_DEF_MIN_SCORE_3D)
                {
                    valid_matches_exist[M.tgt_camID_] = true;
                }
            }

            if(valid_matches_exist.size() > 1)
            {
                scoring_mutex_.lock();
                ++num_valid;
                scoring_mutex_.unlock();
            }
        }

        // check number of segments with valid matches
        valid_f = float(num_valid)/float(v->num_lines());
    }

    //------------------------------------------------------------------------------
    void Line3D::scoringGPU(const unsigned int src, float& valid_f)
    {
#ifdef L3DPP_CUDA
        // INFO: src data must be on GPU! initSrcDataGPU(src) -> remove afterwards!

        // init
        valid_f = 0.0f;
        L3DPP::View* v = views_[src];
        float k = v->k();

        if(num_matches_[src] == 0)
            return;

        // sort matches by ids first
        sortMatches(src);

        // find start and end indices
        L3DPP::DataArray<int2>* ranges = new L3DPP::DataArray<int2>(v->num_lines(),1);
        unsigned int offset = 0;
        for(size_t i=0; i<v->num_lines(); ++i)
        {
            if(matches_[src][i].size() > 0)
            {
                ranges->dataCPU(i,0)[0] = make_int2(offset,offset+matches_[src][i].size()-1);
                offset += matches_[src][i].size();
            }
            else
            {
                // no matches for this segment
                ranges->dataCPU(i,0)[0] = make_int2(-1,-1);
            }
        }

        // store matches in array
        L3DPP::DataArray<float4>* matches = new L3DPP::DataArray<float4>(num_matches_[src],1);
        L3DPP::DataArray<float>* scores = new L3DPP::DataArray<float>(num_matches_[src],1,true);

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            int offset = ranges->dataCPU(i,0)[0].x;
            if(offset >= 0)
            {
                int id = 0;
                std::list<L3DPP::Match>::iterator it = matches_[src][i].begin();
                for(; it!=matches_[src][i].end(); ++it,++id)
                {
                    L3DPP::Match m = *it;
                    matches->dataCPU(offset+id,0)[0] = make_float4(i,m.tgt_camID_,
                                                                   m.depth_p1_,m.depth_p2_);
                }
            }
        }

        // upload
        ranges->upload();
        matches->upload();

        unsigned int num_valid = 0;

        // score on GPU
        L3DPP::score_matches_GPU(v->lines(),matches,ranges,scores,v->RtKinvGPU(),v->C_GPU(),
                                 two_sigA_sqr_,k,L3D_DEF_MIN_SIMILARITY_3D);
        scores->download();

        // write back
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            std::map<unsigned int,bool> valid_matches_exist;
            int offset = ranges->dataCPU(i,0)[0].x;
            if(offset >= 0)
            {
                int id = 0;
                std::list<L3DPP::Match>::iterator it = matches_[src][i].begin();
                for(; it!=matches_[src][i].end(); ++it,++id)
                {
                    // get score
                    float score = scores->dataCPU(offset+id,0)[0];

                    // update
                    (*it).score3D_ = score;

                    if(score > L3D_DEF_MIN_SCORE_3D)
                    {
                        valid_matches_exist[(*it).tgt_camID_] = true;
                    }
                }
            }

            if(valid_matches_exist.size() > 1)
            {
                scoring_mutex_.lock();
                ++num_valid;
                scoring_mutex_.unlock();
            }
        }

        // check number of segments with valid matches
        valid_f = float(num_valid)/float(v->num_lines());

        // cleanup
        delete ranges;
        delete matches;
        delete scores;
#endif //L3DPP_CUDA
    }

    //------------------------------------------------------------------------------
    float Line3D::similarityForScoring(const L3DPP::Match m1, const L3DPP::Match m2,
                                       const float current_k1)
    {
        L3DPP::Segment3D seg3D1 = unprojectMatch(m1,true);
        L3DPP::Segment3D seg3D2 = unprojectMatch(m2,true);

        if(seg3D1.length() < L3D_EPS || seg3D2.length() < L3D_EPS)
            return 0.0f;

        // angular similarity
        float angle = angleBetweenSeg3D(seg3D1,seg3D2,true);
        float sim_a = expf(-angle*angle/two_sigA_sqr_);

        // positional similarity
        float sim_p = 0.0f;
        if(m1.src_camID_ == m2.src_camID_ && m1.src_segID_ == m2.src_segID_)
        {
            // local similarity
            float d1 = m1.depth_p1_-m2.depth_p1_;
            float d2 = m1.depth_p2_-m2.depth_p2_;

            float reg1,reg2;
            float sig1 = m1.depth_p1_*current_k1;
            float sig2 = m1.depth_p2_*current_k1;

            reg1 = 2.0f*sig1*sig1;
            reg2 = 2.0f*sig2*sig2;

            sim_p = fmin(expf(-d1*d1/reg1),expf(-d2*d2/reg2));
        }

        float sim = fmin(sim_a,sim_p);
        if(sim > L3D_DEF_MIN_SIMILARITY_3D)
            return sim;
        else
            return 0.0f;
    }

    //------------------------------------------------------------------------------
    float Line3D::similarity(const L3DPP::Segment2D seg1, const L3DPP::Segment2D seg2,
                             const bool truncate)
    {
        // check for 3D estimates
        if(entry_map_.find(seg1) == entry_map_.end())
        {
            return 0.0f;
        }

        size_t ent1 = entry_map_[seg1];
        std::pair<L3DPP::Segment3D,L3DPP::Match> data1 = estimated_position3D_[ent1];
        L3DPP::Segment3D s1 = data1.first;
        L3DPP::Match m1 = data1.second;

        return similarity(s1,m1,seg2,truncate);
    }

    //------------------------------------------------------------------------------
    float Line3D::similarity(const L3DPP::Segment3D s1, const L3DPP::Match m1,
                             const L3DPP::Segment2D seg2, const bool truncate)
    {
        // check for 3D estimates
        if(entry_map_.find(seg2) == entry_map_.end())
        {
            return 0.0f;
        }

        size_t ent2 = entry_map_[seg2];
        std::pair<L3DPP::Segment3D,L3DPP::Match> data2 = estimated_position3D_[ent2];
        L3DPP::Segment3D s2 = data2.first;
        L3DPP::Match m2 = data2.second;

        if(s1.length() < L3D_EPS || s2.length() < L3D_EPS)
            return 0.0f;

        L3DPP::View* v1 = views_[m1.src_camID_];
        L3DPP::View* v2 = views_[m2.src_camID_];

        /*
        // positional similarity
        Eigen::Vector3d p[2],q[2];
        p[0] = s1.P1(); p[1] = s1.P2();
        q[0] = s2.P1(); q[1] = s2.P2();
        float depth1 = 0.0f;
        float depth2 = 0.0f;
        Eigen::Vector3d lps[2];
        float max_dist = 0.0f;
        Eigen::Vector3d pts[2];

        for(size_t i=0; i<2; ++i)
        {
            for(size_t j=0; j<2; ++j)
            {
                float d = (p[i]-q[j]).norm();
                if(d > max_dist)
                {
                    max_dist = d;
                    lps[0] = p[i]; lps[1] = q[j];
                    pts[0] = p[1-i]; pts[1] = q[1-j];
                    depth1 = (v1->C()-pts[0]).norm();
                    depth2 = (v2->C()-pts[1]).norm();
                }
            }
        }

        L3DPP::Segment3D line(lps[0],lps[1]);
        if(line.length() < L3D_EPS)
            return 0.0f;

        float d1 = line.distance_Point2Line(pts[0]);
        float d2 = line.distance_Point2Line(pts[1]);

        float reg1,reg2;
        float sig1;
        if(depth1 > v1->median_depth())
            sig1 = v1->median_sigma();
        else
            sig1 = depth1*v1->k();

        float sig2;
        if(depth2 > v2->median_depth())
            sig2 = v2->median_sigma();
        else
            sig2 = depth2*v2->k();

        reg1 = 2.0f*sig1*sig1;
        reg2 = 2.0f*sig2*sig2;

        float sim_p = fmin(expf(-d1*d1/reg1),expf(-d2*d2/reg2));

        // angular similarity
        float angle1 = angleBetweenSeg3D(line,s1,true);
        float angle2 = angleBetweenSeg3D(line,s2,true);
        float sim_a = fmin(expf(-angle1*angle1/two_sigA_sqr_),
                           expf(-angle2*angle2/two_sigA_sqr_));
        */

        // angular similarity
        float angle = angleBetweenSeg3D(s1,s2,true);
        float sim_a = expf(-angle*angle/two_sigA_sqr_);

        // positional similarity
        float d11 = s2.distance_Point2Line(s1.P1());
        float d12 = s2.distance_Point2Line(s1.P2());
        float d21 = s1.distance_Point2Line(s2.P1());
        float d22 = s1.distance_Point2Line(s2.P2());

        float reg11,reg12,reg21,reg22;
        float sig11;
        if(m1.depth_p1_ > v1->median_depth())
            sig11 = v1->median_sigma();
        else
            sig11 = m1.depth_p1_*v1->k();

        float sig12;
        if(m1.depth_p2_ > v1->median_depth())
            sig12 = v1->median_sigma();
        else
            sig12 = m1.depth_p2_*v1->k();

        reg11 = 2.0f*sig11*sig11;
        reg12 = 2.0f*sig12*sig12;

        float sig21;
        if(m2.depth_p1_ > v2->median_depth())
            sig21 = v2->median_sigma();
        else
            sig21 = m2.depth_p1_*v2->k();

        float sig22;
        if(m2.depth_p2_ > v2->median_depth())
            sig22 = v2->median_sigma();
        else
            sig22 = m2.depth_p2_*v2->k();

        reg21 = 2.0f*sig21*sig21;
        reg22 = 2.0f*sig22*sig22;

        float sim_p1 = fmin(expf(-d11*d11/reg11),expf(-d12*d12/reg12));
        float sim_p2 = fmin(expf(-d21*d21/reg21),expf(-d22*d22/reg22));

        float sim_p = fmin(sim_p1,sim_p2);

        float sim = fmin(sim_a,sim_p);

        if(truncate)
        {
            if(sim > L3D_DEF_MIN_SIMILARITY_3D)
                return sim;
            else
                return 0.0f;
        }
        return sim;
    }

    //------------------------------------------------------------------------------
    L3DPP::Segment3D Line3D::unprojectMatch(const L3DPP::Match m, const bool src)
    {
        if(src)
        {
            L3DPP::View* v = views_[m.src_camID_];
            return v->unprojectSegment(m.src_segID_,m.depth_p1_,m.depth_p2_);
        }
        else
        {
            L3DPP::View* v = views_[m.tgt_camID_];
            return v->unprojectSegment(m.tgt_segID_,m.depth_q1_,m.depth_q2_);
        }
    }

    //------------------------------------------------------------------------------
    float Line3D::angleBetweenSeg3D(const L3DPP::Segment3D s1, const L3DPP::Segment3D s2,
                                    const bool undirected)
    {
        float dot_p = s1.dir().dot(s2.dir());
        float angle = acos(fmax(fmin(dot_p,1.0f),-1.0f))/M_PI*180.0f;

        if(undirected && angle > 90.0f)
        {
            angle = 180.0f-angle;
        }

        return angle;
    }

    //------------------------------------------------------------------------------
    void Line3D::filterMatches(const unsigned int src)
    {
        // filter and find median depth
        std::vector<float> depths;

        unsigned int num_valid = 0;
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            L3DPP::Match best_match;
            best_match.score3D_ = 0.0f;

            std::list<L3DPP::Match> matches = matches_[src][i];
            matches_[src][i].clear();
            std::list<L3DPP::Match>::iterator it = matches.begin();
            for(; it!=matches.end(); ++it)
            {
                if((*it).score3D_ > L3D_DEF_MIN_SCORE_3D)
                {
                    matches_[src][i].push_back(*it);

                    if((*it).score3D_ > best_match.score3D_)
                        best_match = (*it);
                }
            }

            scoring_mutex_.lock();
            num_valid += matches_[src][i].size();
            scoring_mutex_.unlock();

            // store best match as estimated 3D position
            if(best_match.score3D_ > L3D_DEF_MIN_BEST_SCORE_3D)
            {
                L3DPP::Segment2D seg(src,i);
                L3DPP::Segment3D seg3D = unprojectMatch(best_match,true);
                best_match_mutex_.lock();
                entry_map_[seg] = estimated_position3D_.size();
                estimated_position3D_.push_back(std::pair<L3DPP::Segment3D,L3DPP::Match>(seg3D,best_match));

                // store depths
                depths.push_back(best_match.depth_p1_);
                depths.push_back(best_match.depth_p2_);
                best_match_mutex_.unlock();
            }
        }

        num_matches_[src] = num_valid;

        // median depth
        float med_depth = L3D_EPS;
        if(depths.size() > 0)
        {
            std::sort(depths.begin(),depths.end());
            med_depth = depths[depths.size()/2];
        }

        if(!fixed3Dregularizer_)
            views_[src]->update_median_depth(med_depth,-1.0f);
        else
            views_[src]->update_median_depth(med_depth,sigma_p_);
    }

    //------------------------------------------------------------------------------
    void Line3D::storeInverseMatches(const unsigned int src)
    {
        for(size_t i=0; i<matches_[src].size(); ++i)
        {
            std::list<L3DPP::Match>::iterator it = matches_[src][i].begin();
            for(; it!=matches_[src][i].end(); ++it)
            {
                L3DPP::Match m = *it;
                if(m.score3D_ > 0.0f && !processed_[m.tgt_camID_])
                {
                    L3DPP::Match m_inv;
                    m_inv = m;
                    m_inv.src_camID_ = m.tgt_camID_;
                    m_inv.src_segID_ = m.tgt_segID_;
                    m_inv.tgt_camID_ = m.src_camID_;
                    m_inv.tgt_segID_ = m.src_segID_;
                    m_inv.depth_p1_ = m.depth_q1_;
                    m_inv.depth_p2_ = m.depth_q2_;
                    m_inv.depth_q1_ = m.depth_p1_;
                    m_inv.depth_q2_ = m.depth_p2_;
                    m_inv.score3D_ = 0.0f;

                    matches_[m.tgt_camID_][m.tgt_segID_].push_back(m_inv);
                    ++num_matches_[m.tgt_camID_];
                }
            }
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::reconstruct3Dlines(const unsigned int visibility_t, const bool perform_diffusion,
                                    const float collinearity_t, const bool use_CERES,
                                    const unsigned int max_iter_CERES)
    {
        // no views can be added during reconstruction!
        view_mutex_.lock();
        view_reserve_mutex_.lock();

        std::cout <<std::endl << prefix_ << "[3] RECONSTRUCTION ===============================" << std::endl;

        if(estimated_position3D_.size() == 0)
        {
            std::cout << prefix_wng_ << "no clusterable segments! forgot to match lines?" << std::endl;
            view_reserve_mutex_.unlock();
            view_mutex_.unlock();
            return;
        }

        // init
        max_iter_CERES_ = max_iter_CERES;
        visibility_t_ = std::max(int(visibility_t),3);
        clusters3D_.clear();
        lines3D_.clear();
        float prev_collin_t = collinearity_t_;
        collinearity_t_ = collinearity_t;

#ifdef L3DPP_CUDA
        perform_RDD_ = (perform_diffusion && useGPU_);
        if(perform_diffusion && !useGPU_)
            std::cout << prefix_err_ << "diffusion only possible when GPU mode enabled! using graph clustering instead..." << std::endl;
#else
        perform_RDD_ = false;
        if(perform_diffusion)
            std::cout << prefix_err_ << "diffusion not possible without CUDA! using graph clustering instead..." << std::endl;
#endif //L3DPP_CUDA

#ifdef L3DPP_CERES
        use_CERES_ = use_CERES;
#else
        use_CERES_ = false;
        if(use_CERES)
            std::cout << prefix_err_ << "CERES was not found! no optimization will be performed..." << std::endl;
#endif

        std::cout << prefix_ << "reconstructing 3D lines... [diffusion=" << perform_RDD_ << ", CERES=" << use_CERES_ << "]" << std::endl;

        // find collinear segments (if not already done)
        if(collinearity_t_ > L3D_EPS && (prev_collin_t < L3D_EPS || fabs(prev_collin_t-collinearity_t_) > L3D_EPS))
        {
            std::cout << prefix_ << "find collinear segments... [" << collinearity_t_ <<" px]" << std::endl;
            findCollinearSegments();
        }

        // compute affinity matrix
        std::cout << prefix_ << "computing affinity matrix..." << std::endl;
        computingAffinityMatrix();

        std::cout << prefix_ << "A: ";
        std::cout << "#entries=" << A_.size() << ", #rows=" << global2local_.size();

        unsigned int perc = float(global2local_.size())/float(num_lines_total_)*100.0f;
        std::cout << " [~" << perc << "%]" << std::endl;

        // perform diffusion
        if(perform_RDD_)
        {
            std::cout << prefix_ << "matrix diffusion..." << std::endl;
            performRDD();
        }

        // cluster matrix
        std::cout << prefix_ << "clustering segments..." << std::endl;
        clusterSegments();

        global2local_.clear();
        local2global_.clear();

        // optimize
        if(use_CERES_)
        {
            std::cout << prefix_ << "optimizing 3D lines..." << std::endl;
            optimizeClusters();
        }

        // compute final 3D segments
        std::cout << prefix_ << "computing final 3D lines..." << std::endl;
        computeFinal3Dsegments();

        clusters3D_.clear();

        // filter tiny (noisy) segments
        std::cout << prefix_ << "filtering tiny segments..." << std::endl;
        filterTinySegments();

        std::cout << prefix_ << "3D lines: total=" << lines3D_.size() << std::endl;

        view_reserve_mutex_.unlock();
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::findCollinearSegments()
    {
        if(collinearity_t_ > L3D_EPS)
        {
            std::map<unsigned int,L3DPP::View*>::iterator it=views_.begin();
            unsigned int i=0;
            for(; it!=views_.end(); ++it,++i)
            {
                it->second->findCollinearSegments(collinearity_t_,useGPU_);

                if(i%10 == 0)
                {
                    if(i != 0)
                        std::cout << std::endl;

                    std::cout << prefix_;
                }

                std::cout << "[" << std::setfill('0') << std::setw(L3D_DISP_CAMS) << it->first << "] ";
            }
            std::cout << std::endl;
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::computingAffinityMatrix()
    {
        // reset
        A_.clear();
        global2local_.clear();
        local2global_.clear();
        localID_ = 0;
        used_.clear();

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<estimated_position3D_.size(); ++i)
        {
            L3DPP::Segment3D seg3D = estimated_position3D_[i].first;
            L3DPP::Match m = estimated_position3D_[i].second;
            L3DPP::Segment2D seg2D(m.src_camID_,m.src_segID_);
            bool found_aff = false;
            int id1 = -1;

            // iterate over matches
            std::list<L3DPP::Match>::iterator m_it = matches_[m.src_camID_][m.src_segID_].begin();
            for(; m_it!=matches_[m.src_camID_][m.src_segID_].end(); ++m_it)
            {
                L3DPP::Match m2 = *m_it;
                L3DPP::Segment2D seg2D2(m2.tgt_camID_,m2.tgt_segID_);

                float sim = similarity(seg3D,m,seg2D2,false);

                if(sim > L3D_DEF_MIN_AFFINITY && unused(seg2D,seg2D2))
                {
                    // check IDs
                    if(id1 < 0)
                        id1 = getLocalID(seg2D);

                    int id2 = getLocalID(seg2D2);

                    // push into affinity matrix
                    aff_mat_mutex_.lock();

                    CLEdge e;
                    e.i_ = id1;
                    e.j_ = id2;
                    e.w_ = sim;
                    A_.push_back(e);
                    e.i_ = id2;
                    e.j_ = id1;
                    A_.push_back(e);
                    found_aff = true;

                    aff_mat_mutex_.unlock();

                    // add links to potentially collinear segments to tgt
                    if(collinearity_t_ > L3D_EPS)
                    {
                        L3DPP::View* v = views_[seg2D2.camID()];
                        std::list<unsigned int> coll = v->collinearSegments(seg2D2.segID());

                        std::list<unsigned int>::iterator cit = coll.begin();
                        for(; cit!=coll.end(); ++cit)
                        {
                            L3DPP::Segment2D seg2D2_coll(seg2D2.camID(),*cit);

                            float sim = similarity(seg3D,m,seg2D2_coll,false);

                            if(sim > L3D_DEF_MIN_AFFINITY && unused(seg2D,seg2D2_coll))
                            {
                                // check IDs
                                int id2 = getLocalID(seg2D2_coll);

                                // push into affinity matrix
                                aff_mat_mutex_.lock();

                                CLEdge e;
                                e.i_ = id1;
                                e.j_ = id2;
                                e.w_ = sim;
                                A_.push_back(e);
                                e.i_ = id2;
                                e.j_ = id1;
                                A_.push_back(e);

                                aff_mat_mutex_.unlock();
                            }
                        }
                    }
                }
            }

            // add links to potentially collinear segments
            if(found_aff && id1 >= 0 && collinearity_t_ > L3D_EPS)
            {
                L3DPP::View* v = views_[seg2D.camID()];
                std::list<unsigned int> coll = v->collinearSegments(seg2D.segID());

                std::list<unsigned int>::iterator cit = coll.begin();
                for(; cit!=coll.end(); ++cit)
                {
                    L3DPP::Segment2D seg2D_coll(seg2D.camID(),*cit);

                    float sim = similarity(seg3D,m,seg2D_coll,false);

                    if(sim > L3D_DEF_MIN_AFFINITY && unused(seg2D,seg2D_coll))
                    {
                        // check IDs
                        int id2 = getLocalID(seg2D_coll);

                        // push into affinity matrix
                        aff_mat_mutex_.lock();

                        CLEdge e;
                        e.i_ = id1;
                        e.j_ = id2;
                        e.w_ = sim;
                        A_.push_back(e);
                        e.i_ = id2;
                        e.j_ = id1;
                        A_.push_back(e);

                        aff_mat_mutex_.unlock();
                    }
                }
            }
        }

        // cleanup
        used_.clear();
    }

    //------------------------------------------------------------------------------
    bool Line3D::unused(const L3DPP::Segment2D seg1, const L3DPP::Segment2D seg2)
    {
        bool unused = true;

        // check if used
        aff_used_mutex_.lock();
        if(used_[seg1].find(seg2) != used_[seg1].end())
        {
            // already used
            unused = false;
        }
        else
        {
            // not yet used
            used_[seg1][seg2] = true;
            used_[seg2][seg1] = true;
        }
        aff_used_mutex_.unlock();

        return unused;
    }

    //------------------------------------------------------------------------------
    int Line3D::getLocalID(const L3DPP::Segment2D seg)
    {
        int id;
        aff_id_mutex_.lock();
        if(global2local_.find(seg) == global2local_.end())
        {
            id = localID_;
            ++localID_;

            global2local_[seg] = id;
            local2global_[id] = seg;
        }
        else
        {
            id = global2local_[seg];
        }
        aff_id_mutex_.unlock();
        return id;
    }

    //------------------------------------------------------------------------------
    void Line3D::performRDD()
    {
#ifdef L3DPP_CUDA
        // create sparse GPU matrix
        L3DPP::SparseMatrix* W = new L3DPP::SparseMatrix(A_,global2local_.size());

        // perform RDD
        L3DPP::replicator_dynamics_diffusion_GPU(W,prefix_);

        // update affinities (symmetrify)
        W->download();
        A_.clear();

        std::map<int,std::map<int,float> > entries;
        for(unsigned int i=0; i<W->entries()->width(); ++i)
        {
            int s1 = W->entries()->dataCPU(i,0)[0].x;
            int s2 = W->entries()->dataCPU(i,0)[0].y;
            float w12 = W->entries()->dataCPU(i,0)[0].z;

            float w21 = w12;
            if(entries[s2].find(s1) != entries[s2].end())
            {
                // other one already processed
                w21 = entries[s2][s1];
            }

            float w = fmin(w12,w21);

            entries[s1][s2] = w;
            entries[s2][s1] = w;
        }

        std::map<int,std::map<int,float> >::iterator it = entries.begin();
        for(; it!=entries.end(); ++it)
        {
            std::map<int,float>::iterator it2 = it->second.begin();
            for(; it2!=it->second.end(); ++it2)
            {
                CLEdge e;
                e.i_ = it->first;
                e.j_ = it2->first;
                e.w_ = it2->second;
                A_.push_back(e);
            }
        }

        // cleanup
        delete W;
#endif //L3DPP_CUDA
    }

    //------------------------------------------------------------------------------
    void Line3D::clusterSegments()
    {
        // init
        clusters3D_.clear();
        lines3D_.clear();

        if(A_.size() == 0)
            return;

        // graph clustering
        L3DPP::CLUniverse* u = L3DPP::performClustering(A_,global2local_.size(),3.0f);

        // clustering done
        A_.clear();

        //process clusters
        std::map<int,std::list<L3DPP::Segment2D> > cluster2segments;
        std::map<int,std::map<unsigned int,bool> > cluster2cameras;
        std::vector<int> unique_clusters;

        std::map<int,L3DPP::Segment2D>::iterator it = local2global_.begin();
        for(; it!=local2global_.end(); ++it)
        {
            int clID = u->find(it->first);
            L3DPP::Segment2D seg = it->second;

            if(cluster2segments.find(clID) == cluster2segments.end())
                unique_clusters.push_back(clID);

            // store segment
            cluster2segments[clID].push_back(seg);
            // store camera
            cluster2cameras[clID][seg.camID()] = true;
        }
        delete u;

        if(cluster2segments.size() == 0)
        {
            std::cout << prefix_wng_ << "no clusters found..." << std::endl;
            return;
        }

        std::cout << prefix_ << "clusters: ";
        std::cout << "total=" << cluster2segments.size() << ", ";

        // create 3D lines for valid clusters
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<unique_clusters.size(); ++i)
        {
            int clID = unique_clusters[i];

            if(cluster2cameras[clID].size() >= visibility_t_)
            {
                // create 3D line cluster
                L3DPP::LineCluster3D LC = get3DlineFromCluster(cluster2segments[clID]);

                if(LC.size() > 0)
                {
                    // 3D line valid --> store in list
                    cluster_mutex_.lock();
                    clusters3D_.push_back(LC);
                    cluster_mutex_.unlock();
                }
            }
        }
        std::cout << "valid=" << clusters3D_.size();

        unsigned int perc = float(clusters3D_.size())/float(cluster2segments.size())*100;
        std::cout << " [~" << perc << "%]";

        std::cout << std::endl;
    }

    //------------------------------------------------------------------------------
    L3DPP::LineCluster3D Line3D::get3DlineFromCluster(std::list<L3DPP::Segment2D>& cluster)
    {
        // create scatter matrix
        Eigen::Vector3d P(0,0,0);
        int n = cluster.size()*2;
        Eigen::MatrixXd L_points(3,n);

        std::list<L3DPP::Segment2D>::iterator it = cluster.begin();
        float max_len = 0.0f;
        L3DPP::Segment2D correspondingSeg2D;
        for(size_t i=0; it!=cluster.end(); ++it,i+=2)
        {
            // get 3D hypothesis
            size_t pos = entry_map_[*it];
            L3DPP::Segment3D hyp3D = estimated_position3D_[pos].first;

            P += hyp3D.P1();
            P += hyp3D.P2();

            L_points(0,i) = hyp3D.P1().x();
            L_points(1,i) = hyp3D.P1().y();
            L_points(2,i) = hyp3D.P1().z();

            L_points(0,i+1) = hyp3D.P2().x();
            L_points(1,i+1) = hyp3D.P2().y();
            L_points(2,i+1) = hyp3D.P2().z();

            // corresponding 2D segment -> longest 3D hyp
            if(hyp3D.length() > max_len)
            {
                max_len = hyp3D.length();
                correspondingSeg2D = *it;
            }
        }

        // center of gravity
        P /= double(n);

        // direction
        Eigen::MatrixXd C = Eigen::MatrixXd::Identity(n,n)-(1.0/(double)(n))*Eigen::MatrixXd::Constant(n,n,1.0);
        Eigen::MatrixXd Scat = L_points*C*L_points.transpose();

        Eigen::JacobiSVD<Eigen::MatrixXd> svd(Scat, Eigen::ComputeThinU);

        Eigen::MatrixXd U;
        Eigen::VectorXd S;

        U = svd.matrixU();
        S = svd.singularValues();

        int maxPos;
        S.maxCoeff(&maxPos);

        Eigen::Vector3d dir = Eigen::Vector3d(U(0, maxPos), U(1, maxPos), U(2, maxPos));
        dir.normalize();

        // initial 3D line
        L3DPP::Segment3D initial3Dline(P,P+dir);

        // project corresponding segment
        bool success;
        L3DPP::Segment3D clusterLine = project2DsegmentOnto3Dline(correspondingSeg2D,
                                                                  initial3Dline,success);

        L3DPP::LineCluster3D LC;
        if(success)
        {
            // projection successfull
            LC = L3DPP::LineCluster3D(clusterLine,correspondingSeg2D,
                                      cluster);
        }
        return LC;
    }

    //------------------------------------------------------------------------------
    L3DPP::Segment3D Line3D::project2DsegmentOnto3Dline(const L3DPP::Segment2D seg2D,
                                                        const L3DPP::Segment3D seg3D,
                                                        bool& success)
    {
        // tgt line
        Eigen::Vector3d P = seg3D.P1();
        Eigen::Vector3d u = seg3D.dir();

        // src line 1
        L3DPP::View* v = views_[seg2D.camID()];
        Eigen::Vector3d Q = v->C();
        Eigen::Vector3d v1 = v->getNormalizedLinePointRay(seg2D.segID(),true);

        // src line 2
        Eigen::Vector3d v2 = v->getNormalizedLinePointRay(seg2D.segID(),false);

        Eigen::Vector3d w = P-Q;

        // vals
        double a = u.dot(u);
        double b1 = u.dot(v1);
        double b2 = u.dot(v2);
        double c1 = v1.dot(v1);
        double c2 = v2.dot(v2);
        double d = u.dot(w);
        double e1 = v1.dot(w);
        double e2 = v2.dot(w);

        double denom1 = a*c1 - b1*b1;
        double denom2 = a*c2 - b2*b2;

        if(fabs(denom1) > L3D_EPS && fabs(denom2) > L3D_EPS)
        {
            success = true;
            double s1 = (b1*e1 - c1*d)/denom1;
            double s2 = (b2*e2 - c2*d)/denom2;

            return L3DPP::Segment3D(P+s1*u,P+s2*u);
        }
        else
        {
            // projection not possible
            success = false;
            return L3DPP::Segment3D();
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::optimizeClusters()
    {
#ifdef L3DPP_CERES
        L3DPP::LineOptimizer opt(views_,&clusters3D_,max_iter_CERES_);
        opt.optimize();
#endif //L3DPP_CERES
    }

    //------------------------------------------------------------------------------
    void Line3D::computeFinal3Dsegments()
    {
        // iterate over clusters and find all valid collinear segments
#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<clusters3D_.size(); ++i)
        {
            L3DPP::LineCluster3D LC = clusters3D_[i];
            std::list<L3DPP::Segment3D> collinear = findCollinearSegments(LC);

            if(collinear.size() > 0)
            {
                L3DPP::FinalLine3D final;
                final.collinear3Dsegments_ = collinear;
                final.underlyingCluster_ = LC;

                cluster_mutex_.lock();
                lines3D_.push_back(final);
                cluster_mutex_.unlock();
            }
        }
    }

    //------------------------------------------------------------------------------
    void Line3D::filterTinySegments()
    {
        // remove 3D segments that are too small
        size_t valid_before = lines3D_.size();
        if(valid_before == 0)
            return;

#ifdef L3DPP_OPENMP
        #pragma omp parallel for
#endif //L3DPP_OPENMP
        for(size_t i=0; i<lines3D_.size(); ++i)
        {
            L3DPP::FinalLine3D L = lines3D_[i];
            L3DPP::View* v = views_[L.underlyingCluster_.correspondingSeg2D().camID()];

            std::list<L3DPP::Segment3D> filteredSegments;
            std::list<L3DPP::Segment3D>::iterator it = L.collinear3Dsegments_.begin();
            for(; it!=L.collinear3Dsegments_.end(); ++it)
            {
                if(v->projectedLongEnough(*it))
                    filteredSegments.push_back(*it);
            }

            lines3D_[i].collinear3Dsegments_ = filteredSegments;
        }

        // remove invalid lines
        std::vector<L3DPP::FinalLine3D> lines3D;
        for(size_t i=0; i<lines3D_.size(); ++i)
        {
            if(lines3D_[i].collinear3Dsegments_.size() > 0)
                lines3D.push_back(lines3D_[i]);
        }
        lines3D_ = lines3D;

        size_t valid_after = lines3D_.size();

        std::cout << prefix_ << "removed lines: " << valid_before-valid_after << std::endl;
    }

    //------------------------------------------------------------------------------
    std::list<L3DPP::Segment3D> Line3D::findCollinearSegments(L3DPP::LineCluster3D& cluster)
    {
        // project onto 3D line
        std::list<L3DPP::Segment3D> collinear_segments;
        Eigen::Vector3d COG = 0.5*(cluster.seg3D().P1()+cluster.seg3D().P2());
        std::list<L3DPP::Segment2D>::iterator it = cluster.residuals()->begin();

        std::list<L3DPP::PointOn3DLine> linePoints;
        std::vector<Eigen::Vector3d> pts(cluster.residuals()->size()*2);

        float distToCOG = 0.0f;
        Eigen::Vector3d border;

        size_t pID = 0;
        for(size_t id=0; it!=cluster.residuals()->end(); ++it,++id,pID+=2)
        {
            // project onto 3D line
            bool success;
            L3DPP::Segment3D proj = project2DsegmentOnto3Dline(*it,cluster.seg3D(),success);

            if(success)
            {
                // create line points
                L3DPP::PointOn3DLine p1,p2;

                p1.lineID_ = id;
                p1.pointID_ = pID;
                p1.camID_ = (*it).camID();
                pts[pID] = proj.P1();
                linePoints.push_back(p1);

                float d = (proj.P1()-COG).norm();
                if(d > distToCOG)
                {
                    distToCOG = d;
                    border = proj.P1();
                }

                p2.lineID_ = id;
                p2.pointID_ = pID+1;
                p2.camID_ = (*it).camID();
                pts[pID+1] = proj.P2();
                linePoints.push_back(p2);

                d = (proj.P2()-COG).norm();
                if(d > distToCOG)
                {
                    distToCOG = d;
                    border = proj.P2();
                }
            }
        }

        // check number of projected lines/points
        if(linePoints.size() < 6)
            return collinear_segments;

        // sort by distance to border
        std::list<L3DPP::PointOn3DLine>::iterator lit = linePoints.begin();
        for(; lit!=linePoints.end(); ++lit)
        {
            (*lit).distToBorder_ = (pts[(*lit).pointID_]-border).norm();
        }
        linePoints.sort(L3DPP::sortPointsOn3DLine);

        // iterate and create 3D segments
        std::map<size_t,unsigned int> open;
        std::map<size_t,bool> open_lines;
        bool opened = false;
        Eigen::Vector3d current_start(0,0,0);
        lit = linePoints.begin();
        for(; lit!=linePoints.end(); ++lit)
        {
            L3DPP::PointOn3DLine pt = *lit;

            if(open_lines.find(pt.lineID_) == open_lines.end())
            {
                // opening
                open_lines[pt.lineID_] = true;

                if(open.find(pt.camID_) == open.end())
                    open[pt.camID_] = 1;
                else
                    ++open[pt.camID_];
            }
            else
            {
                // closing
                open_lines.erase(pt.lineID_);

                --open[pt.camID_];

                if(open[pt.camID_] == 0)
                    open.erase(pt.camID_);
            }

            if(opened && open.size() < 3)
            {
                L3DPP::Segment3D l(current_start,pts[pt.pointID_]);
                collinear_segments.push_back(l);
                opened = false;
            }
            else if(!opened && open.size() >= 3)
            {
                current_start = pts[pt.pointID_];
                opened = true;
            }
        }

        return collinear_segments;
    }

    //------------------------------------------------------------------------------
    void Line3D::get3Dlines(std::vector<L3DPP::FinalLine3D>& result)
    {
        view_mutex_.lock();
        view_reserve_mutex_.lock();
        result = lines3D_;
        view_reserve_mutex_.unlock();
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::saveResultAsSTL(const std::string output_folder)
    {
        view_mutex_.lock();
        view_reserve_mutex_.lock();

        if(lines3D_.size() == 0)
        {
            std::cout << prefix_wng_ << "no 3D lines to save!" << std::endl;
            view_reserve_mutex_.unlock();
            view_mutex_.unlock();
            return;
        }

        // get filename
        std::string filename = output_folder+"/"+createOutputFilename()+".stl";

        std::ofstream file;
        file.open(filename.c_str());

        file << "solid lineModel" << std::endl;

        for(size_t i=0; i<lines3D_.size(); ++i)
        {
            L3DPP::FinalLine3D current = lines3D_[i];

            std::list<L3DPP::Segment3D>::iterator it2 = current.collinear3Dsegments_.begin();
            for(; it2!=current.collinear3Dsegments_.end(); ++it2)
            {
                Eigen::Vector3d P1 = (*it2).P1();
                Eigen::Vector3d P2 = (*it2).P2();

                char x1[50];
                char y1[50];
                char z1[50];

                char x2[50];
                char y2[50];
                char z2[50];

                sprintf(x1,"%e",P1.x());
                sprintf(y1,"%e",P1.y());
                sprintf(z1,"%e",P1.z());

                sprintf(x2,"%e",P2.x());
                sprintf(y2,"%e",P2.y());
                sprintf(z2,"%e",P2.z());

                file << " facet normal 1.0e+000 0.0e+000 0.0e+000" << std::endl;
                file << "  outer loop" << std::endl;
                file << "   vertex " << x1 << " " << y1 << " " << z1 << std::endl;
                file << "   vertex " << x2 << " " << y2 << " " << z2 << std::endl;
                file << "   vertex " << x1 << " " << y1 << " " << z1 << std::endl;
                file << "  endloop" << std::endl;
                file << " endfacet" << std::endl;
            }
        }

        file << "endsolid lineModel" << std::endl;
        file.close();

        view_reserve_mutex_.unlock();
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::saveResultAsOBJ(const std::string output_folder)
    {
        view_mutex_.lock();
        view_reserve_mutex_.lock();

        if(lines3D_.size() == 0)
        {
            std::cout << prefix_wng_ << "no 3D lines to save!" << std::endl;
            view_reserve_mutex_.unlock();
            view_mutex_.unlock();
            return;
        }

        // get filename
        std::string filename = output_folder+"/"+createOutputFilename()+".obj";

        std::ofstream file;
        file.open(filename.c_str());

        size_t lineID = 0;
        size_t pointID = 1;
        std::map<size_t,size_t> lines2points;
        for(size_t i=0; i<lines3D_.size(); ++i)
        {
            L3DPP::FinalLine3D current = lines3D_[i];

            std::list<L3DPP::Segment3D>::iterator it2 = current.collinear3Dsegments_.begin();
            for(; it2!=current.collinear3Dsegments_.end(); ++it2,++lineID,pointID+=2)
            {
                Eigen::Vector3d P1 = (*it2).P1();
                Eigen::Vector3d P2 = (*it2).P2();

                file << "v " << P1.x() << " " << P1.y() << " " << P1.z() << std::endl;
                file << "v " << P2.x() << " " << P2.y() << " " << P2.z() << std::endl;

                lines2points[lineID] = pointID;
            }
        }

        std::map<size_t,size_t>::iterator it = lines2points.begin();
        for(; it!=lines2points.end(); ++it)
        {
            file << "l " << it->second << " " << it->second+1 << std::endl;
        }

        file.close();

        view_reserve_mutex_.unlock();
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    void Line3D::save3DLinesAsTXT(const std::string output_folder)
    {
        view_mutex_.lock();
        view_reserve_mutex_.lock();

        if(lines3D_.size() == 0)
        {
            std::cout << prefix_wng_ << "no 3D lines to save!" << std::endl;
            view_reserve_mutex_.unlock();
            view_mutex_.unlock();
            return;
        }

        // get filename
        std::string filename = output_folder+"/"+createOutputFilename()+".txt";

        std::ofstream file;
        file.open(filename.c_str());

        for(size_t i=0; i<lines3D_.size(); ++i)
        {
            L3DPP::FinalLine3D current = lines3D_[i];

            if(current.collinear3Dsegments_.size() == 0)
                continue;

            // write 3D segments
            file << current.collinear3Dsegments_.size() << " ";
            std::list<L3DPP::Segment3D>::iterator it2 = current.collinear3Dsegments_.begin();
            for(; it2!=current.collinear3Dsegments_.end(); ++it2)
            {
                Eigen::Vector3d P1 = (*it2).P1();
                Eigen::Vector3d P2 = (*it2).P2();

                file << P1.x() << " " << P1.y() << " " << P1.z() << " ";
                file << P2.x() << " " << P2.y() << " " << P2.z() << " ";
            }

            // write 2D residuals
            file << current.underlyingCluster_.residuals()->size() << " ";
            std::list<L3DPP::Segment2D>::iterator it3 = current.underlyingCluster_.residuals()->begin();
            for(; it3!=current.underlyingCluster_.residuals()->end(); ++it3)
            {
                file << (*it3).camID() << " " << (*it3).segID() << " ";
                Eigen::Vector4f coords = getSegmentCoords2D(*it3);
                file << coords(0) << " " << coords(1) << " ";
                file << coords(2) << " " << coords(3) << " ";
            }

            file << std::endl;
        }

        file.close();

        view_reserve_mutex_.unlock();
        view_mutex_.unlock();
    }

    //------------------------------------------------------------------------------
    Eigen::Matrix3d Line3D::rotationFromRPY(const double roll, const double pitch,
                                            const double yaw)
    {
        const Eigen::Matrix3d Rx
          = Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitX()).toRotationMatrix();
        const Eigen::Matrix3d Ry
          = Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY()).toRotationMatrix();
        const Eigen::Matrix3d Rz
          = Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ()).toRotationMatrix();

        const Eigen::Matrix3d R = Rz * Ry * Rx;

        return R;

        /*
        Eigen::Vector3d omega(roll,pitch,yaw);
        double theta = omega.norm();
        Eigen::Matrix3d J, J2;

        J <<  0        , -omega(2),  omega(1),
              omega(2) , 0        , -omega(0),
              -omega(1), omega(0) ,    0;

        J2 = J * J;

        double c1, c2;
        if(theta < L3D_EPS)
        {
          c1 = 1 - theta*theta/6.0;
          c2 = 1 - theta*theta/24.0;
        }
        else
        {
          c1 = sin(theta)/theta;
          c2 = (1.0-cos(theta))/(theta*theta);
        }
        return Eigen::Matrix3d::Identity() + c1*J + c2*J2;
        */
    }

    //------------------------------------------------------------------------------
    Eigen::Vector4f Line3D::getSegmentCoords2D(const L3DPP::Segment2D seg2D)
    {
        Eigen::Vector4f coords(0,0,0,0);
        if(views_.find(seg2D.camID()) != views_.end())
        {
            coords = views_[seg2D.camID()]->getLineSegment2D(seg2D.segID());
        }
        return coords;
    }

    //------------------------------------------------------------------------------
    Eigen::Vector4f Line3D::getSegmentCoords2D(const unsigned int camID,
                                               const unsigned int segID)
    {
        return getSegmentCoords2D(L3DPP::Segment2D(camID,segID));
    }

    //------------------------------------------------------------------------------
    void Line3D::eigen2dataArray(L3DPP::DataArray<float>* &DA, const Eigen::MatrixXd M)
    {
        DA = new L3DPP::DataArray<float>(M.cols(),M.rows());
        for(size_t y=0; y<size_t(M.rows()); ++y)
            for(size_t x=0; x<size_t(M.cols()); ++x)
                DA->dataCPU(x,y)[0] = M(y,x);
    }

    //------------------------------------------------------------------------------
    std::string Line3D::createOutputFilename()
    {
        std::stringstream str;
        str << "Line3D++__";

        str << "W_" << max_image_width_ << "__";

        str << "N_" << num_neighbors_ << "__";

        str << "sigmaP_" << sigma_p_ << "__";
        str << "sigmaA_" << sigma_a_ << "__";

        str << "epiOverlap_" << epipolar_overlap_ << "__";

        str << "minBaseline_" << min_baseline_ << "__";

        if(kNN_ > 0)
            str << "kNN_" << kNN_ << "__";

        if(collinearity_t_ > L3D_EPS)
            str << "COLLIN_" << collinearity_t_ << "__";

        if(fixed3Dregularizer_)
            str << "FXD_SIGMA_P__";

        if(perform_RDD_)
            str << "DIFFUSION__";

        if(use_CERES_)
            str << "OPTIMIZED__";

        str << "vis_" << visibility_t_;
        return str.str();
    }
}
