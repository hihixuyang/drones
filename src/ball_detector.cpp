#include "ball_detector.h"

namespace rosdrone_Detector
{

// constructor
ballDetector::ballDetector(const ros::NodeHandle& ng, const ros::NodeHandle& nl) : nhg(ng), nhl(nl)
{
  // initialize communications
  image_transport::ImageTransport it(nhg);
  imageSub = it.subscribe("/image", 2, &ballDetector::imageCallback, this);
  camInfoSub = nhg.subscribe("/camera_info", 2, &ballDetector::camInfoCallback, this);

  imagePub = nhg.advertise<sensor_msgs::Image>("processed_image", 1);
  bearingPub = nhg.advertise<drones::FormationLink>("bearing", 1);

  // initialize values
  sat_ = 130;
  val_ = 100;
  infoDetection.ballRadius = 0.09;
  infoDetection.t_ball2drone = Eigen::Vector3d(0,0,0.15);
  infoDetection.t_camera2drone = Eigen::Vector3d(0.07, 0.0, 0.055);
  getParametersROS();
  outputMessage.drone_name = "drone" + std::to_string(paramsROS.drone_ID);
  show_segment_ = false;
  show_output_ = false;

  if(show_segment_ || show_output_)
  {
    cv::namedWindow("Color detector - range");
    cv::createTrackbar( "Saturation", "Color detector - range", &sat_, 255);
    cv::createTrackbar( "Value", "Color detector - range", &val_, 255);
    cv::setTrackbarPos("Saturation", "Color detector - range", 130);
    cv::setTrackbarPos("Value", "Color detector - range", 95);
  }

  ROS_INFO_STREAM("drone" << paramsROS.drone_ID << "Ball Detector initialized");
}

void ballDetector::spinDetector()
{
  if (img_received)
  {
    std::vector<cv::Vec3f> circles;
    for(auto rgb : paramsROS.drones_color)
    {
        int target_id = rgb.first;
        colorRGB2HUE(rgb.second[0], rgb.second[1], rgb.second[2]);

        cv::Vec3f circle;
        //bool measure = detectColorfulCirclesHUE(img, circle);
        bool measure = process(img,circle);

        kalmanFilterProcess(measure, circle, target_id);

        if (measure)
        {
          addMeasureToOutput(target_id, circle);
          circles.push_back(circle);
        }
    }

    for(auto circle : circles)
    {
      cv::Point center(std::round(circle[0]), std::round(circle[1]));
      int radius = std::round(circle[2]);
      cv::circle(img, center, radius, cv::Scalar(248, 28, 148), 2);
    }
    circles.clear();

    cv::cvtColor(img, img_processed, cv::COLOR_BGR2RGB);
    imagePub.publish(cv_bridge::CvImage(std_msgs::Header(), "rgb8", img_processed).toImageMsg());
    bearingPub.publish(outputMessage);
    outputMessage.bearings.clear();
    outputMessage.targets.clear();
    outputMessage.distances.clear();
  }
}

void ballDetector::colorRGB2HUE(int r, int g, int b)
{
  hue_.clear();
  // convert color to HSV
  const float cmax = std::max(r, std::max(g, b));
  const float cmin = std::min(r, std::min(g, b));
  const float d = cmax - cmin;

  int h = 0;
  if (d)
  {
    if (cmax == r)
      h = 30 * (fmod((g - b) / d, 6));
    else if (cmax == g)
      h = 30 * ((b - r) / d + 2);
    else
      h = 30 * ((r - g) / d + 4);
  }

  // build inRange bounds for hue
  int hthr = 10;
  hue_ = {std::max(h - hthr, 0), std::min(h + hthr, 179)};

  // other segmentation for h
  if (h < hthr)
  {
    hue_.push_back(179 + h - hthr);
    hue_.push_back(179);
  }
  else if (h + hthr > 179)
  {
    hue_.push_back(0);
    hue_.push_back(h + hthr - 179);
  }
}

void ballDetector::imgBGRtoimgHUE(cv::Mat& img)
{
  // Convert input image to HSV
  cv::Mat hsv_image, hue_image;
  cv::cvtColor(img, hsv_image, cv::COLOR_BGR2HSV);

  // Threshold the HSV image, keep only the red pixels
  cv::inRange(hsv_image, cv::Scalar(hue_[0], sat_, val_), cv::Scalar(hue_[1], 255, 255), img);

  if (hue_.size() == 4)
  {
    cv::inRange(hsv_image, cv::Scalar(hue_[2], sat_, val_), cv::Scalar(hue_[3], 255, 255), hue_image);
    cv::addWeighted(img, 1.0, hue_image, 1.0, 0.0, img); // Combine the above two images
  }
}

std::vector<cv::Point> ballDetector::findMainContour(const cv::Mat &_im)
{
    cv::Mat img_, seg1_, seg2_;
    cv::cvtColor(_im, img_, cv::COLOR_BGR2HSV);
    cv::GaussianBlur(img_, img_, cv::Size(11,11), 2);

    if(hue_.size())
    {
        // segment for detection of given RGB (from Hue)
        cv::inRange(img_, cv::Scalar(hue_[0], sat_, val_),
                cv::Scalar(hue_[1], 255, 255), seg1_);
        // add for 2nd detection if near red
        if(hue_.size() == 4)
        {
            cv::inRange(img_, cv::Scalar(hue_[2], sat_, val_),
                    cv::Scalar(hue_[3], 255, 255), seg2_);
            seg1_ += seg2_;
        }

        if(show_segment_)
        {
            cv::imshow("Color detector - range", seg1_);
            if(!show_output_)
                cv::waitKey(1);
        }

        if(show_output_)
        {
            cv::imshow("Color detector output",seg1_);
            cv::waitKey(1);
        }

        std::vector<std::vector<cv::Point> > contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours( seg1_, contours, hierarchy, CV_RETR_CCOMP,
                          CV_CHAIN_APPROX_SIMPLE);

        // pop all children
        bool found = true;
        while(found)
        {
            found = false;
            for(unsigned int i=0;i<hierarchy.size();++i)
            {
                if(hierarchy[i][3] > -1)
                {
                    found = true;
                    hierarchy.erase(hierarchy.begin()+i,hierarchy.begin()+i+1);
                    contours.erase(contours.begin()+i, contours.begin()+i+1);
                    break;
                }
            }
        }

        if(contours.size())
        {
            // get largest contour
            auto largest = std::max_element(
                        contours.begin(), contours.end(),
                        [](const std::vector<cv::Point> &c1, const std::vector<cv::Point> &c2)
            {return cv::contourArea(c1) < cv::contourArea(c2);});
            int idx = std::distance(contours.begin(), largest);

            return contours[idx];
        }
    }
    else
    {
        std::cout << "Color detector: RGB to detect was not defined\n";

    }
    return std::vector<cv::Point>();
}

bool ballDetector::process(const cv::Mat &_im, cv::Vec3f& circle, bool write_output)
{
    auto contour = findMainContour(_im);

    if(!contour.size())
    {
        return false;
    }

    cv::Point2f pt; float radius;
    cv::minEnclosingCircle(contour, pt, radius);

    double area = cv::contourArea(contour);
    if(area > 0.5*M_PI*pow(radius,2) && radius > 10.0)
    {
      circle[0] = pt.x;
      circle[1] = pt.y;
      circle[2] = radius;
      return true;
    }
    else
    {
      return false;
    }
}

bool ballDetector::detectColorfulCirclesHUE(cv::Mat& img,
                                              cv::Vec3f& circle,
                                              bool write_circle)
{
  if (hue_.size())
  {
    // process image
    cv::Mat hue_image = img.clone();

    imgBGRtoimgHUE(hue_image);

    cv::GaussianBlur(hue_image, hue_image, cv::Size(7, 7), 1.5, 1.5);

    // Use the Hough transform to detect circles in the combined threshold image
    std::vector<cv::Vec3f> circles;
    cv::HoughCircles(hue_image, circles, CV_HOUGH_GRADIENT, 1, hue_image.rows / 8, 150, 20, 0, 0);

    // Loop over all detected circles and outline them on the original image
    if (circles.size() == 0) return false;

    auto largest = std::max_element(circles.begin(), circles.end(),
                                    [](const cv::Vec3f& c1, const cv::Vec3f& c2)
                                    { return c1[2] < c2[2]; });
    int idx = std::distance(circles.begin(), largest);
    circle = circles[idx];

    /*double new_radius;
    if (bestRadiusEstimation(new_radius, img, circle) && new_radius <= circle[2])
      circle[2] = new_radius;*/

    if (write_circle)
    {
      cv::Point center(std::round(circle[0]), std::round(circle[1]));
      int radius = std::round(circle[2]);
      cv::circle(img, center, radius, cv::Scalar(248, 24, 148), 1);
    }

    return true;
  }

  return false;
}

bool ballDetector::bestRadiusEstimation(double& radius,
                                          const cv::Mat& image,
                                          const cv::Vec3f& circle)
{

  int x = circle[0];
  int y = circle[1];
  int r = std::round(circle[2]);

  int startX = std::max(0, (int)std::round(x - 1.5 * r));
  int startY = std::max(0, (int)std::round(y - 1.5 * r));
  int endX = std::min(startX + 3 * r, image.cols);
  int endY = std::min(startY + 3 * r, image.rows);

  cv::Mat ROI(image, cv::Rect(startX, startY, endX - startX, endY - startY));
  cv::Mat hue_image;
  ROI.copyTo(hue_image);

  imgBGRtoimgHUE(hue_image);

  cv::GaussianBlur(hue_image, hue_image, cv::Size(3, 3), 0.3, 0.3);

  std::vector<cv::Vec3f> circles;
  cv::HoughCircles(hue_image, circles, CV_HOUGH_GRADIENT, 1, hue_image.rows / 4, 150, 25, 8, 0);

  if (circles.size())
  {
    auto largest = std::max_element(circles.begin(), circles.end(),
                                    [](const cv::Vec3f& c1, const cv::Vec3f& c2)
                                    {
                                      return c1[2] < c2[2];
                                    });
    int idx = std::distance(circles.begin(), largest);

    radius = circles[idx][2];

    return true;
  }

  return false;
}

void ballDetector::kalmanFilterProcess(const bool measure,
                                         cv::Vec3f& circle,
                                         const int& uav_detected_id )
{
  KalmanFilterPtr KF;

  if(getKalmanFilterStructure(KF, measure, uav_detected_id))
  {
    double dT;
    KF->last_estimation = KF->current_estimation;
    KF->current_estimation = ros::Time::now().toSec();
    dT = KF->current_estimation - KF->last_estimation;

    if(KF->found)
    {
      KF->kf.transitionMatrix.at<float>(3) = dT;
      KF->kf.transitionMatrix.at<float>(9) = dT;
      KF->state = KF->kf.predict();
    }

    if (!measure)
    {
      if (ros::Time::now().toSec() - KF->last_measure > 3.0)
        measuresKF.erase(uav_detected_id);
    }
    else
    {
      KF->last_measure = ros::Time::now().toSec();

      KF->meas.at<float>(0) = circle[0];
      KF->meas.at<float>(1) = circle[1];
      KF->meas.at<float>(2) = circle[2];

      if (!KF->found) // First detection!
      {
        // Initialization
        KF->kf.errorCovPre.at<float>(0) = 0.01;
        KF->kf.errorCovPre.at<float>(6) = 0.01;
        KF->kf.errorCovPre.at<float>(12) = 0.01;
        KF->kf.errorCovPre.at<float>(18) = 0.001;
        KF->kf.errorCovPre.at<float>(24) = 0.001;

        KF->state.at<float>(0) = KF->meas.at<float>(0);
        KF->state.at<float>(1) = KF->meas.at<float>(1);
        KF->state.at<float>(2) = KF->meas.at<float>(2);
        KF->state.at<float>(3) = 0;
        KF->state.at<float>(4) = 0;

        KF->kf.statePost = KF->state;

        KF->found = true;
      }
      else
        KF->kf.correct(KF->meas); // Kalman Correction
    }

    circle[0] = KF->state.at<float>(0);
    circle[1] = KF->state.at<float>(1);
    circle[2] = KF->state.at<float>(2);
  }

}

bool ballDetector::getKalmanFilterStructure(KalmanFilterPtr& KFPtr,
                                            const bool measure,
                                            const int& uav_detected_id)
{
  std::map<int, KalmanFilterPtr>::iterator it = measuresKF.find(uav_detected_id);

  if (it == measuresKF.end())
  {
    if(measure)
      measuresKF[uav_detected_id] = std::make_shared<KalmanFilter>();
    else
      return false;
  }

  KFPtr =  measuresKF[uav_detected_id];
  return true;
}

bool ballDetector::addMeasureToOutput(const int& target_id, const cv::Vec3f& circle)
{
  if (!camInfo.received) { ROS_ERROR("Camera information not found!"); return false; }

  outputMessage.targets.push_back( "drone" + std::to_string(target_id) );
  double distance = camInfo.K(0, 0) * infoDetection.ballRadius / circle[2];

  Eigen::Vector3d P;
  P << circle[0], circle[1], 1;

  Eigen::Vector3d bearingRaw;
  bearingRaw = camInfo.R * camInfo.K.inverse() * P;

  // Transformation from ball frame to drone frame
  Eigen::Vector3d bearing = bearingRaw * distance - infoDetection.t_ball2drone;

  // Transformation from camera frame to drone frame
  bearing = infoDetection.t_camera2drone + bearing;

  // Update distance
  std_msgs::Float64 distanceMessage;
  distanceMessage.data = bearing.norm();
  outputMessage.distances.push_back(distanceMessage);

  bearing.normalize();

  geometry_msgs::Vector3 bearingMessage;
  tf::vectorEigenToMsg(bearing, bearingMessage);

  outputMessage.bearings.push_back(bearingMessage);

  return true;
}

void ballDetector::getParametersROS()
{
  if (nhl.getParam("uav_id", paramsROS.drone_ID) && nhg.getParam("/uavs_info/num_uavs", paramsROS.num_uavs))
  {
    for (int i = 0; i < paramsROS.num_uavs; i++)
    {
      int id;
      int r, g, b;
      nhg.getParam("/uavs_info/uav_" + std::to_string(i + 1) + "/id", id);
      if (id != paramsROS.drone_ID)
      {
        std::vector<int> rgb(3);
        nhg.getParam("/uavs_info/uav_" + std::to_string(i + 1) + "/r", rgb[0]);
        nhg.getParam("/uavs_info/uav_" + std::to_string(i + 1) + "/g", rgb[1]);
        nhg.getParam("/uavs_info/uav_" + std::to_string(i + 1) + "/b", rgb[2]);
        paramsROS.drones_color[id] = rgb;
      }
    }
  }
  else
  {
    ROS_ERROR("Some ROS parameters were not found! (uav_id and/or "
              "/uavs_info/num_uavs)");
  }
}

// callback functions

void ballDetector::camInfoCallback(const sensor_msgs::CameraInfo& _camInfo)
{
  camInfo.K <<  _camInfo.K[0], _camInfo.K[1], _camInfo.K[2],
                _camInfo.K[3], _camInfo.K[4], _camInfo.K[5],
                _camInfo.K[6], _camInfo.K[7], _camInfo.K[8];

  camInfo.R << 0, 0, 1,
              -1, 0, 0,
              0, -1, 0;

  camInfo.width = _camInfo.width;
  camInfo.height = _camInfo.height;

  camInfo.received = true;
}

void ballDetector::imageCallback(const sensor_msgs::ImageConstPtr& image)
{
  try
  {
    img = cv_bridge::toCvCopy(image, "bgr8")->image;
    if (!img.empty())
      img_received = true;
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("Could not convert from '%s' to 'bgr8'.", image->encoding.c_str());
  }
}

}
