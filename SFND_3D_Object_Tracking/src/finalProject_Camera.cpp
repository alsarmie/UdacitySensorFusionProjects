
/* INCLUDES FOR THIS PROJECT */
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/xfeatures2d/nonfree.hpp>
#include <sstream>
#include <vector>

#include "../include/camFusion.hpp"
#include "../include/dataStructures.h"
#include "../include/lidarData.hpp"
#include "../include/matching2D.hpp"
#include "../include/objectDetection2D.hpp"

using namespace std;

const string DETECTOR =
    "AKAZE"; // SHITOMASI, HARRIS, FAST, BRISK, ORB, AKAZE, SIFT
const string EXTRACTOR = "AKAZE"; // BRISK, BRIEF, ORB, FREAK, AKAZE, SIFT
const string MATCHER = "MAT_BF";  // MAT_BF, MAT_FLANN
const string NN = "SEL_KNN";      // SEL_NN or SEL_KNN
/* MAIN PROGRAM */
int main(int argc, const char *argv[]) {
  /* INIT VARIABLES AND DATA STRUCTURES */
  std::cout << "Detector " << DETECTOR << " Extractor " << EXTRACTOR
            << " Matcher " << MATCHER << " NN " << NN << std::endl;

  // data location
  string dataPath = "../";

  // camera
  string imgBasePath = dataPath + "images/";
  string imgPrefix =
      "KITTI/2011_09_26/image_02/data/000000"; // left camera, color
  string imgFileType = ".png";
  int imgStartIndex = 0; // first file index to load (assumes Lidar and camera
                         // names have identical naming convention)
  int imgEndIndex = 76;  // last file index to load
  int imgStepWidth = 1;
  // no. of digits which make up the file index (e.g. img-0001.png)
  int imgFillWidth = 4;

  // object detection
  string yoloBasePath = dataPath + "dat/yolo/";
  string yoloClassesFile = yoloBasePath + "coco.names";
  string yoloModelConfiguration = yoloBasePath + "yolov3.cfg";
  string yoloModelWeights = yoloBasePath + "yolov3.weights";

  // Lidar
  string lidarPrefix = "KITTI/2011_09_26/velodyne_points/data/000000";
  string lidarFileType = ".bin";

  // calibration data for camera and lidar
  // rotation matrix and translation vector
  cv::Mat RT = (cv::Mat_<double>(4, 4) << 7.533745e-03, -9.999714e-01,
                -6.166020e-04, -4.069766e-03, 1.480249e-02, 7.280733e-04,
                -9.998902e-01, -7.631618e-02, 9.998621e-01, 7.523790e-03,
                1.480755e-02, -2.717806e-01, 0.0, 0.0, 0.0, 1.0);
  // 3x3 rectifying rotation to
  // make image planes co-planar
  cv::Mat R_rect_00 =
      (cv::Mat_<double>(4, 4) << 9.999239e-01, 9.837760e-03, -7.445048e-03, 0.0,
       -9.869795e-03, 9.999421e-01, -4.278459e-03, 0.0, 7.402527e-03,
       4.351614e-03, 9.999631e-01, 0.0, 0.0, 0.0, 0.0, 1.0);
  // 3x4 projection matrix after rectification
  cv::Mat P_rect_00 =
      (cv::Mat_<double>(3, 4) << 7.215377e+02, 0.0, 6.095593e+02, 0.0, 0.0,
       7.215377e+02, 1.728540e+02, 0.0, 0.0, 0.0, 1.0, 0.0);

  // misc
  double sensorFrameRate =
      10.0 / imgStepWidth;      // frames per second for Lidar and camera
  int dataBufferSize = 3;       // no. of images which are held in memory (ring
                                // buffer) at the same time
  vector<DataFrame> dataBuffer; // list of data frames which are held in memory
                                // at the same time
  bool bVis = true;             // visualize results
  bool counterOn = true;
  short count = 0;
  int kptCount = 0;
  float timeCount = 0.0;
  int matchedCount = 0;

  /* MAIN LOOP OVER ALL IMAGES */

  for (size_t imgIndex = 0; imgIndex <= imgEndIndex - imgStartIndex;
       imgIndex += imgStepWidth) {
    /* LOAD IMAGE INTO BUFFER */

    // assemble filenames for current index
    ostringstream imgNumber;
    imgNumber << setfill('0') << setw(imgFillWidth) << imgStartIndex + imgIndex;
    string imgFullFilename =
        imgBasePath + imgPrefix + imgNumber.str() + imgFileType;

    // load image from file
    cv::Mat img = cv::imread(imgFullFilename);

    // push image into data frame buffer
    DataFrame frame;
    frame.cameraImg = img;
    dataBuffer.push_back(frame);

    if (dataBuffer.size() > dataBufferSize) {
      dataBuffer.erase(dataBuffer.begin());
    }

    cout << "#1 : LOAD IMAGE INTO BUFFER done, size:  " << dataBuffer.size()
         << endl;

    /* DETECT & CLASSIFY OBJECTS */

    float confThreshold = 0.2;
    float nmsThreshold = 0.4;
    bVis = false;
    detectObjects((dataBuffer.end() - 1)->cameraImg,
                  (dataBuffer.end() - 1)->boundingBoxes, confThreshold,
                  nmsThreshold, yoloBasePath, yoloClassesFile,
                  yoloModelConfiguration, yoloModelWeights, bVis);

    cout << "#2 : DETECT & CLASSIFY OBJECTS done" << endl;

    /* CROP LIDAR POINTS */

    // load 3D Lidar points from file
    string lidarFullFilename =
        imgBasePath + lidarPrefix + imgNumber.str() + lidarFileType;
    std::vector<LidarPoint> lidarPoints;
    loadLidarFromFile(lidarPoints, lidarFullFilename);

    // remove Lidar points based on distance properties
    float minZ = -1.5, maxZ = -0.9, minX = 2.0, maxX = 20.0, maxY = 2.0,
          minR = 0.1; // focus on ego lane
    cropLidarPoints(lidarPoints, minX, maxX, maxY, minZ, maxZ, minR);

    (dataBuffer.end() - 1)->lidarPoints = lidarPoints;

    cout << "#3 : CROP LIDAR POINTS done" << endl;

    /* CLUSTER LIDAR POINT CLOUD */

    // associate Lidar points with camera-based ROI
    float shrinkFactor =
        0.10; // shrinks each bounding box by the given percentage to avoid 3D
              // object merging at the edges of an ROI
    clusterLidarWithROI((dataBuffer.end() - 1)->boundingBoxes,
                        (dataBuffer.end() - 1)->lidarPoints, shrinkFactor,
                        P_rect_00, R_rect_00, RT);
    // Visualize 3D objects
    bVis = false;
    if (bVis) {
      show3DObjects((dataBuffer.end() - 1)->boundingBoxes, cv::Size(4.0, 20.0),
                    cv::Size(2000, 2000), true);
    }
    bVis = false;

    cout << "#4 : CLUSTER LIDAR POINT CLOUD done" << endl;

    // REMOVE THIS LINE BEFORE PROCEEDING WITH THE FINAL PROJECT
    // skips directly to the next image without processing what comes beneath

    /* DETECT IMAGE KEYPOINTS */

    // convert current image to grayscale
    cv::Mat imgGray;
    cv::cvtColor((dataBuffer.end() - 1)->cameraImg, imgGray,
                 cv::COLOR_BGR2GRAY);

    // extract 2D keypoints from current image
    vector<cv::KeyPoint>
        keypoints; // create empty feature list for current image
    string detectorType = DETECTOR;

    if (detectorType == "SHITOMASI") {
      detKeypointsShiTomasi(keypoints, imgGray, timeCount, false);
    } else if (detectorType == "HARRIS") {
      detKeypointsHarris(keypoints, imgGray, timeCount, false);
    } else {
      detKeypointsModern(keypoints, imgGray, detectorType, timeCount, false);
    }
    // optional : limit number of keypoints (helpful for debugging and learning)
    bool bLimitKpts = false;
    if (bLimitKpts) {
      int maxKeypoints = 50;

      if (detectorType ==
          "SHITOMASI") { // there is no response info, so keep the first 50 as
                         // they are sorted in descending quality order
        keypoints.erase(keypoints.begin() + maxKeypoints, keypoints.end());
      }
      cv::KeyPointsFilter::retainBest(keypoints, maxKeypoints);
      cout << " NOTE: Keypoints have been limited!" << endl;
    }

    // push keypoints and descriptor for current frame to end of data buffer
    (dataBuffer.end() - 1)->keypoints = keypoints;

    cout << "#5 : DETECT KEYPOINTS done" << endl;

    //* EXTRACT KEYPOINT DESCRIPTORS

    cv::Mat descriptors;
    string extractorType = EXTRACTOR; // BRIEF, ORB, FREAK, AKAZE, SIFT
    descKeypoints((dataBuffer.end() - 1)->keypoints,
                  (dataBuffer.end() - 1)->cameraImg, descriptors, extractorType,
                  timeCount);

    // push descriptors for current frame to end of data buffer
    (dataBuffer.end() - 1)->descriptors = descriptors;

    cout << "#6 : EXTRACT DESCRIPTORS done" << endl;

    if (dataBuffer.size() >
        1) // wait until at least two images have been processed
    {

      //* MATCH KEYPOINT DESCRIPTORS

      vector<cv::DMatch> matches;
      string matcherType = MATCHER; // MAT_BF, MAT_FLANN
      string descriptorType{};
      if (extractorType == "SIFT") {
        descriptorType = "DES_HOG";
      } else {
        descriptorType = "DES_BINARY"; // DES_BINARY, DES_HOG
      }

      string selectorType = NN; // SEL_NN, SEL_KNN

      //// STUDENT ASSIGNMENT
      //// TASK MP.5 -> add FLANN matching in file matching2D.cpp
      //// TASK MP.6 -> add KNN match selection and perform descriptor distance
      /// ratio filtering with t=0.8 in file matching2D.cpp

      matchDescriptors((dataBuffer.end() - 2)->keypoints,
                       (dataBuffer.end() - 1)->keypoints,
                       (dataBuffer.end() - 2)->descriptors,
                       (dataBuffer.end() - 1)->descriptors, matches,
                       descriptorType, matcherType, selectorType);

      //// EOF STUDENT ASSIGNMENT

      // store matches in current data frame
      (dataBuffer.end() - 1)->kptMatches = matches;

      cout << "#7 : MATCH KEYPOINT DESCRIPTORS done" << endl;

      //* TRACK 3D OBJECT BOUNDING BOXES

      //// STUDENT ASSIGNMENT
      //// TASK FP.1 -> match list of 3D objects (vector<BoundingBox>) between
      /// current and previous frame (implement ->matchBoundingBoxes)
      map<int, int> bbBestMatches;
      matchBoundingBoxes(
          matches, bbBestMatches, *(dataBuffer.end() - 2),
          *(dataBuffer.end() - 1)); // associate bounding boxes between current
                                    // and previous frame using keypoint matches
      //// EOF STUDENT ASSIGNMENT
      // cout << "Number 8 " << aux << endl;
      // cout << "Number 8 " << bbBestMatches.size() << endl;

      // store matches in current data frame
      (dataBuffer.end() - 1)->bbMatches = bbBestMatches;

      cout << "#8 : TRACK 3D OBJECT BOUNDING BOXES done, size: "
           << bbBestMatches.size() << endl;

      //* COMPUTE TTC ON OBJECT IN FRONT

      if (dataBuffer.size() > dataBufferSize - 1) {

        // loop over all BB match pairs
        for (auto it1 = (dataBuffer.end() - 1)->bbMatches.begin();
             it1 != (dataBuffer.end() - 1)->bbMatches.end(); ++it1) {

          std::cout.flush();
          // find bounding boxes associates with current match
          BoundingBox *prevBB, *currBB;
          for (auto it2 = (dataBuffer.end() - 1)->boundingBoxes.begin();
               it2 != (dataBuffer.end() - 1)->boundingBoxes.end(); ++it2) {
            // cout<<"XXXX Size: "<<it2->kptMatches.size() << endl;
            if (it1->second == it2->boxID) // check wether current match partner
                                           // corresponds to this BB
            {
              currBB = &(*it2);
              // cout<<"XXXX Size: "<<it2->kptMatches.size() << endl;
            }
          }

          for (auto it2 = (dataBuffer.end() - 2)->boundingBoxes.begin();
               it2 != (dataBuffer.end() - 2)->boundingBoxes.end(); ++it2) {
            if (it1->first == it2->boxID) // check wether current match partner
                                          // corresponds to this BB
            {
              prevBB = &(*it2);
              // cout<<"YYY Size: "<<it2->kptMatches.size() << endl;
            }
          }

          // compute TTC for current match
          if (!currBB->lidarPoints.empty() &&
              !prevBB->lidarPoints
                   .empty()) // only compute TTC if we have Lidar points
          {
            //// STUDENT ASSIGNMENT
            //// TASK FP.2 -> compute time-to-collision based on Lidar data
            ///(implement -> computeTTCLidar)
            double ttcLidar;

            computeTTCLidar(prevBB->lidarPoints, currBB->lidarPoints,
                            sensorFrameRate, ttcLidar);
            //// EOF STUDENT ASSIGNMENT

            //// STUDENT ASSIGNMENT
            //// TASK FP.3 -> assign enclosed keypoint matches to bounding box
            ///(implement -> clusterKptMatchesWithROI) / TASK FP.4 -> compute
            /// time-to-collision based on camera (implement ->
            /// computeTTCCamera)
            double ttcCamera;
            // cout<<"1 Size: "<<currBB->kptMatches.size() << endl;
            clusterKptMatchesWithROI(*currBB, (dataBuffer.end() - 2)->keypoints,
                                     (dataBuffer.end() - 1)->keypoints,
                                     (dataBuffer.end() - 1)->kptMatches);
            // cout<<"Size: "<<currBB->kptMatches.size() << endl;
            computeTTCCamera((dataBuffer.end() - 2)->keypoints,
                             (dataBuffer.end() - 1)->keypoints,
                             currBB->kptMatches, sensorFrameRate, ttcCamera);
            //// EOF STUDENT ASSIGNMENT

            bVis = true;
            if (bVis) {
              cv::Mat visImg = (dataBuffer.end() - 1)->cameraImg.clone();
              showLidarImgOverlay(visImg, currBB->lidarPoints, P_rect_00,
                                  R_rect_00, RT, &visImg);
              cv::rectangle(visImg, cv::Point(currBB->roi.x, currBB->roi.y),
                            cv::Point(currBB->roi.x + currBB->roi.width,
                                      currBB->roi.y + currBB->roi.height),
                            cv::Scalar(0, 255, 0), 2);

              char str[200];
              sprintf(str, "TTC Lidar : %f s, TTC Camera : %f s", ttcLidar,
                      ttcCamera);
              putText(visImg, str, cv::Point2f(80, 50), cv::FONT_HERSHEY_PLAIN,
                      2, cv::Scalar(0, 0, 255));

              string windowName = "Final Results : TTC";
              cv::namedWindow(windowName, 4);
              cv::imshow(windowName, visImg);
              cout << "Press key to continue to next frame" << endl;
              cv::waitKey(0);
              //if ( (char)27 == (char) cv::waitKey(1) ) break;
            }
            bVis = false;

          } // eof TTC computation

        } // eof loop over all BB matches
      }
    }
    //*/
  } // eof loop over all images

  return 0;
}