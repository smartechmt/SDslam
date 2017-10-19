/**
 *
 *  Copyright (C) 2017 Eduardo Perdices <eperdices at gsyc dot es>
 *
 *  The following code is a derivative work of the code from the ORB-SLAM2 project,
 *  which is licensed under the GNU Public License, version 3. This code therefore
 *  is also licensed under the terms of the GNU Public License, version 3.
 *  For more information see <https://github.com/raulmur/ORB_SLAM2>.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SD_SLAM_FRAMEDRAWER_H
#define SD_SLAM_FRAMEDRAWER_H

#include <mutex>
#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>
#include "Tracking.h"
#include "MapPoint.h"
#include "Map.h"

namespace SD_SLAM {

class Tracking;
class Viewer;

class FrameDrawer {
 public:
  FrameDrawer(Map* pMap);

  // Update info from the last processed frame.
  void Update(Tracking *pTracker);

  // Draw last processed frame.
  cv::Mat DrawFrame();

 protected:
  void DrawTextInfo(cv::Mat &im, int nState, cv::Mat &imText);

  void GetInitialPlane(Tracking *pTracker);

  bool Project(const Frame &frame, const Eigen::Matrix<double, 3, 4> &planeRT, const Eigen::Vector3d &p3d, Eigen::Vector2d &p2d);

  // Info of the frame to be drawn
  cv::Mat mIm;
  int N;
  std::vector<cv::KeyPoint> mvCurrentKeys;
  std::vector<bool> mvbMap;
  int mnTracked;
  std::vector<cv::KeyPoint> mvIniKeys;
  std::vector<int> mvIniMatches;
  int mState;

  Map* mpMap;

  std::mutex mMutex;

  std::vector<cv::Point> ARPoints_;
};

}  // namespace SD_SLAM

#endif  // SD_SLAM_FRAMEDRAWER_H
