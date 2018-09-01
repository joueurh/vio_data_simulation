//
// Created by hyj on 17-6-22.
//

#include <fstream>

#include "../src/imu.h"
#include "../src/utilities.h"
#include <opencv2/opencv.hpp>

std::vector < std::pair< Eigen::Vector4d, Eigen::Vector4d > >
CreatePointsLines(std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> >& points)
{

    std::vector < std::pair< Eigen::Vector4d, Eigen::Vector4d > > lines;

    std::ifstream f;
    f.open("../bin/house_model/house.txt");

    while(!f.eof())
    {
        std::string s;
        std::getline(f,s);
        if(!s.empty())
        {
            std::stringstream ss;
            ss << s;
            double x,y,z;
            ss >> x;
            ss >> y;
            ss >> z;
            Eigen::Vector4d pt0( x, y, z, 1 );
            ss >> x;
            ss >> y;
            ss >> z;
            Eigen::Vector4d pt1( x, y, z, 1 );

            bool isHistoryPoint = false;
            for (int i = 0; i < points.size(); ++i) {
                Eigen::Vector4d pt = points[i];
                if(pt == pt0)
                {
                    isHistoryPoint = true;
                }
            }
            if(!isHistoryPoint)
                points.push_back(pt0);

            isHistoryPoint = false;
            for (int i = 0; i < points.size(); ++i) {
                Eigen::Vector4d pt = points[i];
                if(pt == pt1)
                {
                    isHistoryPoint = true;
                }
            }
            if(!isHistoryPoint)
                points.push_back(pt1);

            // pt0 = Twl * pt0;
            // pt1 = Twl * pt1;
            lines.push_back( std::make_pair(pt0,pt1) );   // lines
        }
    }

    // create more 3d points, you can comment this code
    int n = points.size();
    for (int j = 0; j < n; ++j) {
        Eigen::Vector4d p = points[j] + Eigen::Vector4d(0.5,0.5,-0.5,0);
        points.push_back(p);
    }

    // save points
    std::stringstream filename;
    filename<<"all_points.txt";
    save_points(filename.str(),points);
    return lines;
}

std::vector<cv::Mat> GenerateImages(const cv::Mat &image, const std::vector< MotionData > &camdata)
{
	Param params;
	std::vector<cv::Mat> vimageWarps;
	if (!image.data)
	{
		std::cerr << "no image is input" << std::endl;
	}

	Eigen::Matrix3d K;
	K << params.fx, 0.0, params.cx,
		0.0, params.fy, params.cy,
		0.0, 0.0, 1.0;
	std::vector<Eigen::Vector4d> points;
	double edge = 12;
	points.emplace_back(-edge, edge * 3 / 4, 0, 1);
	points.emplace_back(edge, edge * 3 / 4, 0, 1);
	points.emplace_back(edge, -edge * 3 / 4, 0, 1);
	points.emplace_back(-edge, -edge * 3 / 4, 0, 1);
	std::vector<cv::Point2d> src_pts;
	src_pts.emplace_back(0, 0);
	src_pts.emplace_back(640, 0);
	src_pts.emplace_back(640, 480);
	src_pts.emplace_back(0, 480);
	// points obs in image
	for (int n = 0; n < camdata.size(); ++n)
	{
		std::vector<cv::Point2d> dst_pts;
		MotionData data = camdata[n];
		Eigen::Matrix4d T_w_c = Eigen::Matrix4d::Identity();
		T_w_c.block(0, 0, 3, 3) = data.Rwb;
		T_w_c.block(0, 3, 3, 1) = data.twb;

		// 遍历所有的特征点，看哪些特征点在视野里
		for (int i = 0; i < points.size(); ++i) {
			Eigen::Vector4d pw = points[i];//points[i];          // 最后一位存着feature id
			Eigen::Vector4d pc = T_w_c.inverse() * pw; // T_wc.inverse() * Pw  -- > point in cam frame
			Eigen::Vector3d image_pt = { pc[0] / pc[3],pc[1] / pc[3] ,pc[2] / pc[3] };
			image_pt = K * image_pt;
			image_pt /= image_pt[2];
			dst_pts.emplace_back(image_pt[0], image_pt[1]);
		}
		cv::Mat H = cv::findHomography(src_pts, dst_pts);
		cv::Mat imageWarp;
		warpPerspective(image, imageWarp, H, imageWarp.size());
		cv::imshow("affine image", imageWarp);
		cv::waitKey(1);
		vimageWarps.push_back(imageWarp);
	}

	return std::move(vimageWarps);
}


int main(){

    // 生成3d points
    std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> > points;
    std::vector < std::pair< Eigen::Vector4d, Eigen::Vector4d > > lines;
    lines = CreatePointsLines(points);

    // IMU model
    Param params;
    IMU imuGen(params);

    // create imu data
    // imu pose gyro acc
    std::vector< MotionData > imudata;
    std::vector< MotionData > imudata_noise;
    for (float t = params.t_start; t<params.t_end;) {
        MotionData data = imuGen.MotionModel(t);
        imudata.push_back(data);

        // add imu noise
        MotionData data_noise = data;
        imuGen.addIMUnoise(data_noise);
        imudata_noise.push_back(data_noise);

        t += 1.0/params.imu_frequency;
    }
    imuGen.init_velocity_ = imudata[0].imu_velocity;
    imuGen.init_twb_ = imudata.at(0).twb;
    imuGen.init_Rwb_ = imudata.at(0).Rwb;
    save_Pose("imu_pose.txt",imudata);
    save_Pose("imu_pose_noise.txt",imudata_noise);

    imuGen.testImu("imu_pose.txt", "imu_int_pose.txt");     // test the imu data, integrate the imu data to generate the imu trajecotry
    imuGen.testImu("imu_pose_noise.txt", "imu_int_pose_noise.txt");

    // cam pose
    std::vector< MotionData > camdata;
    for (float t = params.t_start; t<params.t_end;) {

        MotionData imu = imuGen.MotionModel(t);   // imu body frame to world frame motion
        MotionData cam;

        cam.timestamp = imu.timestamp;
        cam.Rwb = imu.Rwb * params.R_bc;    // cam frame in world frame
        cam.twb = imu.twb + imu.Rwb * params.t_bc; //  Tcw = Twb * Tbc ,  t = Rwb * tbc + twb

        camdata.push_back(cam);
        t += 1.0/params.cam_frequency;
    }
    save_Pose("cam_pose.txt",camdata);
    save_Pose_asTUM("cam_pose_tum.txt",camdata);

	auto image_files = GenerateImages(cv::imread("../bin/template.jpg"), camdata);
    // points obs in image
    for(int n = 0; n < camdata.size(); ++n)
    {
        MotionData data = camdata[n];
        Eigen::Matrix4d Twc = Eigen::Matrix4d::Identity();
        Twc.block(0, 0, 3, 3) = data.Rwb;
        Twc.block(0, 3, 3, 1) = data.twb;

        // 遍历所有的特征点，看哪些特征点在视野里
        std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> > points_cam;    // ３维点在当前cam视野里
        std::vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d> > features_cam;  // 对应的２维图像坐标
        for (int i = 0; i < points.size(); ++i) {
            Eigen::Vector4d pw = points[i];          // 最后一位存着feature id
            pw[3] = 1;                               //改成齐次坐标最后一位
            Eigen::Vector4d pc1 = Twc.inverse() * pw; // T_wc.inverse() * Pw  -- > point in cam frame

            if(pc1(2) < 0) continue; // z必须大于０,在摄像机坐标系前方

            Eigen::Vector2d obs(pc1(0)/pc1(2), pc1(1)/pc1(2)) ;
//            if( (obs(0)*460 + 255) < params.image_h && ( obs(0) * 460 + 255) > 0 &&
//                    (obs(1)*460 + 255) > 0 && ( obs(1)* 460 + 255) < params.image_w )
            {
                points_cam.push_back(points[i]);
                features_cam.push_back(obs);
            }
        }

        // save points
        std::stringstream filename1;
        filename1<<"../bin/keyframe/all_points_"<<n<<".txt";
        save_features(filename1.str(),points_cam,features_cam);
    }

    // lines obs in image
    for(int n = 0; n < camdata.size(); ++n)
    {
        MotionData data = camdata[n];
        Eigen::Matrix4d Twc = Eigen::Matrix4d::Identity();
        Twc.block(0, 0, 3, 3) = data.Rwb;
        Twc.block(0, 3, 3, 1) = data.twb;

        // 遍历所有的特征点，看哪些特征点在视野里
//        std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> > points_cam;    // ３维点在当前cam视野里
        std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> > features_cam;  // 对应的２维图像坐标
        for (int i = 0; i < lines.size(); ++i) {
            std::pair< Eigen::Vector4d, Eigen::Vector4d > linept = lines[i];

            Eigen::Vector4d pc1 = Twc.inverse() * linept.first; // T_wc.inverse() * Pw  -- > point in cam frame
            Eigen::Vector4d pc2 = Twc.inverse() * linept.second; // T_wc.inverse() * Pw  -- > point in cam frame

            if(pc1(2) < 0 || pc2(2) < 0) continue; // z必须大于０,在摄像机坐标系前方

            Eigen::Vector4d obs(pc1(0)/pc1(2), pc1(1)/pc1(2),
                                pc2(0)/pc2(2), pc2(1)/pc2(2));
            //if(obs(0) < params.image_h && obs(0) > 0 && obs(1)> 0 && obs(1) < params.image_w)
            {
                features_cam.push_back(obs);
            }
        }

        // save points
        std::stringstream filename1;
        filename1<<"keyframe/all_lines_"<<n<<".txt";
        save_lines(filename1.str(),features_cam);
    }


    return 1;
}
