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

#include "Tracking.h"

#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>

#include "ORBmatcher.h"
#include "FrameDrawer.h"
#include "Converter.h"
#include "Map.h"
#include "Initializer.h"
#include "Optimizer.h"
#include "ImageAlign.h"

#include <iostream>
#include <mutex>
#include <unistd.h>

using namespace std;

namespace ORB_SLAM2 {

Tracking::Tracking(System *pSys, FrameDrawer *pFrameDrawer, MapDrawer *pMapDrawer, Map *pMap,
                   const string &strSettingPath, const int sensor):
  mState(NO_IMAGES_YET), mSensor(sensor),
  mpInitializer(static_cast<Initializer*>(NULL)), mpSystem(pSys), mpViewer(NULL),
  mpFrameDrawer(pFrameDrawer), mpMapDrawer(pMapDrawer), mpMap(pMap), mnLastRelocFrameId(0) {
  // Load camera parameters from settings file
  cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
  float fx = fSettings["Camera.fx"];
  float fy = fSettings["Camera.fy"];
  float cx = fSettings["Camera.cx"];
  float cy = fSettings["Camera.cy"];

  cv::Mat K = cv::Mat::eye(3,3,CV_32F);
  K.at<float>(0,0) = fx;
  K.at<float>(1,1) = fy;
  K.at<float>(0,2) = cx;
  K.at<float>(1,2) = cy;
  K.copyTo(mK);

  cv::Mat DistCoef(4,1,CV_32F);
  DistCoef.at<float>(0) = fSettings["Camera.k1"];
  DistCoef.at<float>(1) = fSettings["Camera.k2"];
  DistCoef.at<float>(2) = fSettings["Camera.p1"];
  DistCoef.at<float>(3) = fSettings["Camera.p2"];
  const float k3 = fSettings["Camera.k3"];
  if (k3!=0)
  {
    DistCoef.resize(5);
    DistCoef.at<float>(4) = k3;
  }
  DistCoef.copyTo(mDistCoef);

  mbf = fSettings["Camera.bf"];

  float fps = fSettings["Camera.fps"];
  if (fps==0)
    fps=30;

  // Max/Min Frames to insert keyframes and to check relocalisation
  mMinFrames = 0;
  mMaxFrames = fps;

  cout << endl << "Camera Parameters: " << endl;
  cout << "- fx: " << fx << endl;
  cout << "- fy: " << fy << endl;
  cout << "- cx: " << cx << endl;
  cout << "- cy: " << cy << endl;
  cout << "- k1: " << DistCoef.at<float>(0) << endl;
  cout << "- k2: " << DistCoef.at<float>(1) << endl;
  if (DistCoef.rows==5)
    cout << "- k3: " << DistCoef.at<float>(4) << endl;
  cout << "- p1: " << DistCoef.at<float>(2) << endl;
  cout << "- p2: " << DistCoef.at<float>(3) << endl;
  cout << "- fps: " << fps << endl;


  int nRGB = fSettings["Camera.RGB"];
  mbRGB = nRGB;

  if (mbRGB)
    cout << "- color order: RGB (ignored if grayscale)" << endl;
  else
    cout << "- color order: BGR (ignored if grayscale)" << endl;

  // Load ORB parameters

  int nFeatures = fSettings["ORBextractor.nFeatures"];
  float fScaleFactor = fSettings["ORBextractor.scaleFactor"];
  int nLevels = fSettings["ORBextractor.nLevels"];
  int fIniThFAST = fSettings["ORBextractor.iniThFAST"];
  int fMinThFAST = fSettings["ORBextractor.minThFAST"];

  mpORBextractorLeft = new ORBextractor(nFeatures,fScaleFactor,nLevels,fIniThFAST,fMinThFAST);

  if (sensor==System::MONOCULAR)
    mpIniORBextractor = new ORBextractor(2*nFeatures,fScaleFactor,nLevels,fIniThFAST,fMinThFAST);

  cout << endl  << "ORB Extractor Parameters: " << endl;
  cout << "- Number of Features: " << nFeatures << endl;
  cout << "- Scale Levels: " << nLevels << endl;
  cout << "- Scale Factor: " << fScaleFactor << endl;
  cout << "- Initial Fast Threshold: " << fIniThFAST << endl;
  cout << "- Minimum Fast Threshold: " << fMinThFAST << endl;

  if (sensor==System::RGBD) {
    mThDepth = mbf*(float)fSettings["ThDepth"]/fx;
    cout << endl << "Depth Threshold (Close/Far Points): " << mThDepth << endl;

    mDepthMapFactor = fSettings["DepthMapFactor"];
    if (fabs(mDepthMapFactor)<1e-5)
      mDepthMapFactor=1;
    else
      mDepthMapFactor = 1.0f/mDepthMapFactor;
  }

  threshold_ = 8;
}

void Tracking::SetLocalMapper(LocalMapping *pLocalMapper) {
  mpLocalMapper=pLocalMapper;
}

void Tracking::SetLoopClosing(LoopClosing *pLoopClosing) {
  mpLoopClosing=pLoopClosing;
}

void Tracking::SetViewer(Viewer *pViewer) {
  mpViewer=pViewer;
}

cv::Mat Tracking::GrabImageRGBD(const cv::Mat &imRGB,const cv::Mat &imD, const double &timestamp) {
  mImGray = imRGB;
  cv::Mat imDepth = imD;

  if (mImGray.channels()==3) {
    if (mbRGB)
      cvtColor(mImGray,mImGray,CV_RGB2GRAY);
    else
      cvtColor(mImGray,mImGray,CV_BGR2GRAY);
  } else if (mImGray.channels()==4) {
    if (mbRGB)
      cvtColor(mImGray,mImGray,CV_RGBA2GRAY);
    else
      cvtColor(mImGray,mImGray,CV_BGRA2GRAY);
  }

  if ((fabs(mDepthMapFactor-1.0f)>1e-5) || imDepth.type()!=CV_32F)
    imDepth.convertTo(imDepth,CV_32F,mDepthMapFactor);

  mCurrentFrame = Frame(mImGray,imDepth,timestamp,mpORBextractorLeft,mK,mDistCoef,mbf,mThDepth);

  Track();

  return mCurrentFrame.mTcw.clone();
}


cv::Mat Tracking::GrabImageMonocular(const cv::Mat &im, const double &timestamp) {
  mImGray = im;

  if (mImGray.channels()==3) {
    if (mbRGB)
      cvtColor(mImGray,mImGray,CV_RGB2GRAY);
    else
      cvtColor(mImGray,mImGray,CV_BGR2GRAY);
  }
  else if (mImGray.channels()==4) {
    if (mbRGB)
      cvtColor(mImGray,mImGray,CV_RGBA2GRAY);
    else
      cvtColor(mImGray,mImGray,CV_BGRA2GRAY);
  }

  if (mState==NOT_INITIALIZED || mState==NO_IMAGES_YET)
    mCurrentFrame = Frame(mImGray,timestamp,mpIniORBextractor,mK,mDistCoef,mbf,mThDepth);
  else
    mCurrentFrame = Frame(mImGray,timestamp,mpORBextractorLeft,mK,mDistCoef,mbf,mThDepth);

  Track();

  return mCurrentFrame.mTcw.clone();
}

void Tracking::Track() {
  if (mState==NO_IMAGES_YET)
    mState = NOT_INITIALIZED;

  mLastProcessedState=mState;

  // Get Map Mutex -> Map cannot be changed
  unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

  if (mState==NOT_INITIALIZED) {
    if (mSensor==System::RGBD)
      StereoInitialization();
    else
      MonocularInitialization();

    mpFrameDrawer->Update(this);

    if (mState!=OK)
      return;
  } else {
    // System is initialized. Track Frame.
    bool bOK;

    // Initial camera pose estimation using motion model or relocalization (if tracking is lost)
    if (mState==OK) {
      // Local Mapping might have changed some MapPoints tracked in last frame
      CheckReplacedInLastFrame();

      if (mVelocity.empty() || mCurrentFrame.mnId<mnLastRelocFrameId+2) {
        bOK = TrackReferenceKeyFrame();
      } else {
        bOK = TrackWithMotionModel();
        if (!bOK) {
          bOK = TrackReferenceKeyFrame();
        }
      }
    } else {
      bOK = Relocalization();
    }

    mCurrentFrame.mpReferenceKF = mpReferenceKF;

    // If we have an initial estimation of the camera pose and matching. Track the local map.
    if (bOK)
      bOK = TrackLocalMap();

    if (bOK)
      mState = OK;
    else
      mState=LOST;

    // Update drawer
    mpFrameDrawer->Update(this);

    // If tracking were good, check if we insert a keyframe
    if (bOK)
    {
      // Update motion model
      if (!mLastFrame.mTcw.empty()) {
        cv::Mat LastTwc = cv::Mat::eye(4,4,CV_32F);
        mLastFrame.GetRotationInverse().copyTo(LastTwc.rowRange(0,3).colRange(0,3));
        mLastFrame.GetCameraCenter().copyTo(LastTwc.rowRange(0,3).col(3));
        mVelocity = mCurrentFrame.mTcw*LastTwc;
      } else
        mVelocity = cv::Mat();

      mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

      // Clean VO matches
      for (int i=0; i<mCurrentFrame.N; i++) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
        if (pMP)
          if (pMP->Observations()<1) {
            mCurrentFrame.mvbOutlier[i] = false;
            mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
          }
      }

      // Delete temporal MapPoints
      for (list<MapPoint*>::iterator lit = mlpTemporalPoints.begin(), lend =  mlpTemporalPoints.end(); lit!=lend; lit++)
      {
        MapPoint* pMP = *lit;
        delete pMP;
      }
      mlpTemporalPoints.clear();

      // Check if we need to insert a new keyframe
      if (NeedNewKeyFrame())
        CreateNewKeyFrame();

      // We allow points with high innovation (considererd outliers by the Huber Function)
      // pass to the new keyframe, so that bundle adjustment will finally decide
      // if they are outliers or not. We don't want next frame to estimate its position
      // with those points so we discard them in the frame.
      for (int i=0; i<mCurrentFrame.N;i++)
      {
        if (mCurrentFrame.mvpMapPoints[i] && mCurrentFrame.mvbOutlier[i])
          mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
      }
    }

    // Reset if the camera get lost soon after initialization
    if (mState==LOST)
    {
      if (mpMap->KeyFramesInMap()<=5)
      {
        cout << "Track lost soon after initialisation, reseting..." << endl;
        mpSystem->Reset();
        return;
      }
    }

    if (!mCurrentFrame.mpReferenceKF)
      mCurrentFrame.mpReferenceKF = mpReferenceKF;

    mLastFrame = Frame(mCurrentFrame);
  }

  // Store frame pose information to retrieve the complete camera trajectory afterwards.
  if (!mCurrentFrame.mTcw.empty()) {
    cv::Mat Tcr = mCurrentFrame.mTcw*mCurrentFrame.mpReferenceKF->GetPoseInverse();
    mlRelativeFramePoses.push_back(Tcr);
    mlpReferences.push_back(mpReferenceKF);
    mlFrameTimes.push_back(mCurrentFrame.mTimeStamp);
    mlbLost.push_back(mState==LOST);
  } else {
    // This can happen if tracking is lost
    mlRelativeFramePoses.push_back(mlRelativeFramePoses.back());
    mlpReferences.push_back(mlpReferences.back());
    mlFrameTimes.push_back(mlFrameTimes.back());
    mlbLost.push_back(mState==LOST);
  }

}


void Tracking::StereoInitialization() {
  if (mCurrentFrame.N>500) {
    // Set Frame pose to the origin
    mCurrentFrame.SetPose(cv::Mat::eye(4,4,CV_32F));

    // Create KeyFrame
    KeyFrame* pKFini = new KeyFrame(mCurrentFrame,mpMap);

    // Insert KeyFrame in the map
    mpMap->AddKeyFrame(pKFini);

    // Create MapPoints and asscoiate to KeyFrame
    for (int i=0; i<mCurrentFrame.N;i++)
    {
      float z = mCurrentFrame.mvDepth[i];
      if (z>0)
      {
        cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
        MapPoint* pNewMP = new MapPoint(x3D,pKFini,mpMap);
        pNewMP->AddObservation(pKFini,i);
        pKFini->AddMapPoint(pNewMP,i);
        pNewMP->ComputeDistinctiveDescriptors();
        pNewMP->UpdateNormalAndDepth();
        mpMap->AddMapPoint(pNewMP);

        mCurrentFrame.mvpMapPoints[i]=pNewMP;
      }
    }

    cout << "New map created with " << mpMap->MapPointsInMap() << " points" << endl;

    mpLocalMapper->InsertKeyFrame(pKFini);

    mLastFrame = Frame(mCurrentFrame);
    mnLastKeyFrameId=mCurrentFrame.mnId;
    mpLastKeyFrame = pKFini;

    mvpLocalKeyFrames.push_back(pKFini);
    mvpLocalMapPoints=mpMap->GetAllMapPoints();
    mpReferenceKF = pKFini;
    mCurrentFrame.mpReferenceKF = pKFini;

    mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

    mpMap->mvpKeyFrameOrigins.push_back(pKFini);

    mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

    mState=OK;
  }
}

void Tracking::MonocularInitialization()
{

  if (!mpInitializer)
  {
    // Set Reference Frame
    if (mCurrentFrame.mvKeys.size()>100)
    {
      mInitialFrame = Frame(mCurrentFrame);
      mLastFrame = Frame(mCurrentFrame);
      mvbPrevMatched.resize(mCurrentFrame.mvKeysUn.size());
      for (size_t i=0; i<mCurrentFrame.mvKeysUn.size(); i++)
        mvbPrevMatched[i]=mCurrentFrame.mvKeysUn[i].pt;

      if (mpInitializer)
        delete mpInitializer;

      mpInitializer =  new Initializer(mCurrentFrame,1.0,200);

      fill(mvIniMatches.begin(),mvIniMatches.end(),-1);

      return;
    }
  }
  else
  {
    // Try to initialize
    if ((int)mCurrentFrame.mvKeys.size()<=100)
    {
      delete mpInitializer;
      mpInitializer = static_cast<Initializer*>(NULL);
      fill(mvIniMatches.begin(),mvIniMatches.end(),-1);
      return;
    }

    // Find correspondences
    ORBmatcher matcher(0.9,true);
    int nmatches = matcher.SearchForInitialization(mInitialFrame,mCurrentFrame,mvbPrevMatched,mvIniMatches,100);

    // Check if there are enough correspondences
    if (nmatches<100)
    {
      delete mpInitializer;
      mpInitializer = static_cast<Initializer*>(NULL);
      return;
    }

    cv::Mat Rcw; // Current Camera Rotation
    cv::Mat tcw; // Current Camera Translation
    vector<bool> vbTriangulated; // Triangulated Correspondences (mvIniMatches)

    if (mpInitializer->Initialize(mCurrentFrame, mvIniMatches, Rcw, tcw, mvIniP3D, vbTriangulated))
    {
      for (size_t i=0, iend=mvIniMatches.size(); i<iend;i++)
      {
        if (mvIniMatches[i]>=0 && !vbTriangulated[i])
        {
          mvIniMatches[i]=-1;
          nmatches--;
        }
      }

      // Set Frame Poses
      mInitialFrame.SetPose(cv::Mat::eye(4,4,CV_32F));
      cv::Mat Tcw = cv::Mat::eye(4,4,CV_32F);
      Rcw.copyTo(Tcw.rowRange(0,3).colRange(0,3));
      tcw.copyTo(Tcw.rowRange(0,3).col(3));
      mCurrentFrame.SetPose(Tcw);

      CreateInitialMapMonocular();
    }
  }
}

void Tracking::CreateInitialMapMonocular() {
  // Create KeyFrames
  KeyFrame* pKFini = new KeyFrame(mInitialFrame,mpMap);
  KeyFrame* pKFcur = new KeyFrame(mCurrentFrame,mpMap);

  // Insert KFs in the map
  mpMap->AddKeyFrame(pKFini);
  mpMap->AddKeyFrame(pKFcur);

  // Create MapPoints and asscoiate to keyframes
  for (size_t i=0; i<mvIniMatches.size();i++) {
    if (mvIniMatches[i]<0)
      continue;

    //Create MapPoint.
    cv::Mat worldPos(mvIniP3D[i]);

    MapPoint* pMP = new MapPoint(worldPos,pKFcur,mpMap);

    pKFini->AddMapPoint(pMP,i);
    pKFcur->AddMapPoint(pMP,mvIniMatches[i]);

    pMP->AddObservation(pKFini,i);
    pMP->AddObservation(pKFcur,mvIniMatches[i]);

    pMP->ComputeDistinctiveDescriptors();
    pMP->UpdateNormalAndDepth();

    //Fill Current Frame structure
    mCurrentFrame.mvpMapPoints[mvIniMatches[i]] = pMP;
    mCurrentFrame.mvbOutlier[mvIniMatches[i]] = false;

    //Add to Map
    mpMap->AddMapPoint(pMP);
  }

  // Update Connections
  pKFini->UpdateConnections();
  pKFcur->UpdateConnections();

  // Bundle Adjustment
  cout << "New Map created with " << mpMap->MapPointsInMap() << " points" << endl;

  Optimizer::GlobalBundleAdjustemnt(mpMap,20);

  // Set median depth to 1
  float medianDepth = pKFini->ComputeSceneMedianDepth(2);
  float invMedianDepth = 1.0f/medianDepth;

  if (medianDepth<0 || pKFcur->TrackedMapPoints(1)<100)
  {
    cout << "Wrong initialization, reseting..." << endl;
    Reset();
    return;
  }

  // Scale initial baseline
  cv::Mat Tc2w = pKFcur->GetPose();
  Tc2w.col(3).rowRange(0,3) = Tc2w.col(3).rowRange(0,3)*invMedianDepth;
  pKFcur->SetPose(Tc2w);

  // Scale points
  vector<MapPoint*> vpAllMapPoints = pKFini->GetMapPointMatches();
  for (size_t iMP=0; iMP<vpAllMapPoints.size(); iMP++)
  {
    if (vpAllMapPoints[iMP])
    {
      MapPoint* pMP = vpAllMapPoints[iMP];
      pMP->SetWorldPos(pMP->GetWorldPos()*invMedianDepth);
    }
  }

  mpLocalMapper->InsertKeyFrame(pKFini);
  mpLocalMapper->InsertKeyFrame(pKFcur);

  mCurrentFrame.SetPose(pKFcur->GetPose());
  mnLastKeyFrameId=mCurrentFrame.mnId;
  mpLastKeyFrame = pKFcur;

  mvpLocalKeyFrames.push_back(pKFcur);
  mvpLocalKeyFrames.push_back(pKFini);
  mvpLocalMapPoints=mpMap->GetAllMapPoints();
  mpReferenceKF = pKFcur;
  mCurrentFrame.mpReferenceKF = pKFcur;

  mLastFrame = Frame(mCurrentFrame);

  mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

  mpMapDrawer->SetCurrentCameraPose(pKFcur->GetPose());

  mpMap->mvpKeyFrameOrigins.push_back(pKFini);

  mState=OK;
}

void Tracking::CheckReplacedInLastFrame()
{
  for (int i =0; i<mLastFrame.N; i++)
  {
    MapPoint* pMP = mLastFrame.mvpMapPoints[i];

    if (pMP)
    {
      MapPoint* pRep = pMP->GetReplaced();
      if (pRep)
      {
        mLastFrame.mvpMapPoints[i] = pRep;
      }
    }
  }
}


bool Tracking::TrackReferenceKeyFrame() {
  ORBmatcher matcher(0.7,true);

  // Set same pose
  mCurrentFrame.SetPose(mLastFrame.mTcw);

  // Align current and last image
  ImageAlign image_align;
  if (!image_align.ComputePose(mCurrentFrame, mpReferenceKF)) {
    std::cerr << "[ERROR] Image align failed" << endl;
    return false;
  }

  fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));

  // Project points seen in reference keyframe
  int nmatches = matcher.SearchByProjection(mCurrentFrame,mpReferenceKF,threshold_,mSensor==System::MONOCULAR);

  // If few matches, uses a wider window search
  if (nmatches<20) {
    std::cout << "[DEBUG] Not enough matches, double threshold" << std::endl;
    fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));
    nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2*threshold_, mSensor==System::MONOCULAR);
  }

  if (nmatches<20)
    return false;

  // Optimize frame pose with all matches
  Optimizer::PoseOptimization(&mCurrentFrame);

  // Discard outliers
  int nmatchesMap = 0;
  for (int i =0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (mCurrentFrame.mvbOutlier[i]) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

        mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
        mCurrentFrame.mvbOutlier[i]=false;
        pMP->mbTrackInView = false;
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        nmatches--;
      } else if (mCurrentFrame.mvpMapPoints[i]->Observations()>0)
        nmatchesMap++;
    }
  }

  return nmatchesMap>=10;
}

void Tracking::UpdateLastFrame() {
  // Update pose according to reference keyframe
  KeyFrame* pRef = mLastFrame.mpReferenceKF;
  cv::Mat Tlr = mlRelativeFramePoses.back();

  mLastFrame.SetPose(Tlr*pRef->GetPose());
}

bool Tracking::TrackWithMotionModel() {
  ORBmatcher matcher(0.9,true);

  // Update last frame pose according to its reference keyframe
  UpdateLastFrame();

  if (mVelocity.empty())
    mCurrentFrame.SetPose(mLastFrame.mTcw);
  else
    mCurrentFrame.SetPose(mVelocity*mLastFrame.mTcw);

  // Align current and last image
  ImageAlign image_align;
  if (!image_align.ComputePose(mCurrentFrame,mLastFrame)) {
    std::cerr << "[ERROR] Image align failed" << endl;
    return false;
  }

  fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));

  // Project points seen in previous frame
  int nmatches = matcher.SearchByProjection(mCurrentFrame,mLastFrame,threshold_,mSensor==System::MONOCULAR);

  // If few matches, uses a wider window search
  if (nmatches<20) {
    std::cout << "[DEBUG] Not enough matches, double threshold" << std::endl;
    fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));
    nmatches = matcher.SearchByProjection(mCurrentFrame,mLastFrame,2*threshold_,mSensor==System::MONOCULAR);
  }

  if (nmatches<20)
    return false;

  // Optimize frame pose with all matches
  Optimizer::PoseOptimization(&mCurrentFrame);

  // Discard outliers
  int nmatchesMap = 0;
  for (int i =0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (mCurrentFrame.mvbOutlier[i]) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

        mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
        mCurrentFrame.mvbOutlier[i]=false;
        pMP->mbTrackInView = false;
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        nmatches--;
      } else if (mCurrentFrame.mvpMapPoints[i]->Observations()>0)
        nmatchesMap++;
    }
  }

  return nmatchesMap>=10;
}

bool Tracking::TrackLocalMap() {
  // We have an estimation of the camera pose and some map points tracked in the frame.
  // We retrieve the local map and try to find matches to points in the local map.

  UpdateLocalMap();

  SearchLocalPoints();

  // Optimize Pose
  Optimizer::PoseOptimization(&mCurrentFrame);
  mnMatchesInliers = 0;

  // Update MapPoints Statistics
  for (int i=0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (!mCurrentFrame.mvbOutlier[i]) {
        mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
        if (mCurrentFrame.mvpMapPoints[i]->Observations()>0)
          mnMatchesInliers++;
      }
    }
  }

  // Decide if the tracking was succesful
  // More restrictive if there was a relocalization recently
  if (mCurrentFrame.mnId<mnLastRelocFrameId+mMaxFrames && mnMatchesInliers<50)
    return false;

  if (mnMatchesInliers<30)
    return false;
  else
    return true;
}


bool Tracking::NeedNewKeyFrame() {
  // If Local Mapping is freezed by a Loop Closure do not insert keyframes
  if (mpLocalMapper->isStopped() || mpLocalMapper->stopRequested())
    return false;

  const int nKFs = mpMap->KeyFramesInMap();

  // Do not insert keyframes if not enough frames have passed from last relocalisation
  if (mCurrentFrame.mnId<mnLastRelocFrameId+mMaxFrames && nKFs>mMaxFrames)
    return false;

  // Tracked MapPoints in the reference keyframe
  int nMinObs = 3;
  if (nKFs<=2)
    nMinObs=2;
  int nRefMatches = mpReferenceKF->TrackedMapPoints(nMinObs);

  // Local Mapping accept keyframes?
  bool bLocalMappingIdle = mpLocalMapper->AcceptKeyFrames();

  // Check how many "close" points are being tracked and how many could be potentially created.
  int nNonTrackedClose = 0;
  int nTrackedClose= 0;
  if (mSensor!=System::MONOCULAR) {
    for (int i =0; i<mCurrentFrame.N; i++) {
      if (mCurrentFrame.mvDepth[i]>0 && mCurrentFrame.mvDepth[i]<mThDepth)
      {
        if (mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
          nTrackedClose++;
        else
          nNonTrackedClose++;
      }
    }
  }

  bool bNeedToInsertClose = (nTrackedClose<100) && (nNonTrackedClose>70);

  // Thresholds
  float thRefRatio = 0.75f;
  if (nKFs<2)
    thRefRatio = 0.4f;

  if (mSensor==System::MONOCULAR)
    thRefRatio = 0.9f;

  // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
  const bool c1a = mCurrentFrame.mnId>=mnLastKeyFrameId+mMaxFrames;
  // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
  const bool c1b = (mCurrentFrame.mnId>=mnLastKeyFrameId+mMinFrames && bLocalMappingIdle);
  //Condition 1c: tracking is weak
  const bool c1c =  mSensor!=System::MONOCULAR && (mnMatchesInliers<nRefMatches*0.25 || bNeedToInsertClose) ;
  // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
  const bool c2 = ((mnMatchesInliers<nRefMatches*thRefRatio|| bNeedToInsertClose) && mnMatchesInliers>15);

  if ((c1a||c1b||c1c)&&c2) {
    // If the mapping accepts keyframes, insert keyframe.
    // Otherwise send a signal to interrupt BA
    if (bLocalMappingIdle) {
      return true;
    }
    else {
      mpLocalMapper->InterruptBA();
      if (mSensor!=System::MONOCULAR)
      {
        if (mpLocalMapper->KeyframesInQueue()<3)
          return true;
        else
          return false;
      }
      else
        return false;
    }
  }
  else
    return false;
}

void Tracking::CreateNewKeyFrame()
{
  if (!mpLocalMapper->SetNotStop(true))
    return;

  KeyFrame* pKF = new KeyFrame(mCurrentFrame,mpMap);

  mpReferenceKF = pKF;
  mCurrentFrame.mpReferenceKF = pKF;

  if (mSensor!=System::MONOCULAR) {
    mCurrentFrame.UpdatePoseMatrices();

    // We sort points by the measured depth by the stereo/RGBD sensor.
    // We create all those MapPoints whose depth < mThDepth.
    // If there are less than 100 close points we create the 100 closest.
    vector<pair<float,int> > vDepthIdx;
    vDepthIdx.reserve(mCurrentFrame.N);
    for (int i=0; i<mCurrentFrame.N; i++) {
      float z = mCurrentFrame.mvDepth[i];
      if (z>0) {
        vDepthIdx.push_back(make_pair(z,i));
      }
    }

    if (!vDepthIdx.empty())
    {
      sort(vDepthIdx.begin(),vDepthIdx.end());

      int nPoints = 0;
      for (size_t j=0; j<vDepthIdx.size();j++)
      {
        int i = vDepthIdx[j].second;

        bool bCreateNew = false;

        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
        if (!pMP)
          bCreateNew = true;
        else if (pMP->Observations()<1)
        {
          bCreateNew = true;
          mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
        }

        if (bCreateNew)
        {
          cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
          MapPoint* pNewMP = new MapPoint(x3D,pKF,mpMap);
          pNewMP->AddObservation(pKF,i);
          pKF->AddMapPoint(pNewMP,i);
          pNewMP->ComputeDistinctiveDescriptors();
          pNewMP->UpdateNormalAndDepth();
          mpMap->AddMapPoint(pNewMP);

          mCurrentFrame.mvpMapPoints[i]=pNewMP;
          nPoints++;
        }
        else
        {
          nPoints++;
        }

        if (vDepthIdx[j].first>mThDepth && nPoints>100)
          break;
      }
    }
  }

  mpLocalMapper->InsertKeyFrame(pKF);

  mpLocalMapper->SetNotStop(false);

  mnLastKeyFrameId = mCurrentFrame.mnId;
  mpLastKeyFrame = pKF;
}

void Tracking::SearchLocalPoints()
{
  // Do not search map points already matched
  for (vector<MapPoint*>::iterator vit=mCurrentFrame.mvpMapPoints.begin(), vend=mCurrentFrame.mvpMapPoints.end(); vit!=vend; vit++)
  {
    MapPoint* pMP = *vit;
    if (pMP)
    {
      if (pMP->isBad())
      {
        *vit = static_cast<MapPoint*>(NULL);
      }
      else
      {
        pMP->IncreaseVisible();
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        pMP->mbTrackInView = false;
      }
    }
  }

  int nToMatch=0;

  // Project points in frame and check its visibility
  for (vector<MapPoint*>::iterator vit=mvpLocalMapPoints.begin(), vend=mvpLocalMapPoints.end(); vit!=vend; vit++)
  {
    MapPoint* pMP = *vit;
    if (pMP->mnLastFrameSeen == mCurrentFrame.mnId)
      continue;
    if (pMP->isBad())
      continue;
    // Project (this fills MapPoint variables for matching)
    if (mCurrentFrame.isInFrustum(pMP,0.5))
    {
      pMP->IncreaseVisible();
      nToMatch++;
    }
  }

  if (nToMatch>0)
  {
    ORBmatcher matcher(0.8);
    int th = 1;
    if (mSensor==System::RGBD)
      th=3;
    // If the camera has been relocalised recently, perform a coarser search
    if (mCurrentFrame.mnId<mnLastRelocFrameId+2)
      th=5;
    matcher.SearchByProjection(mCurrentFrame,mvpLocalMapPoints,th);
  }
}

void Tracking::UpdateLocalMap()
{
  // This is for visualization
  mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

  // Update
  UpdateLocalKeyFrames();
  UpdateLocalPoints();
}

void Tracking::UpdateLocalPoints()
{
  mvpLocalMapPoints.clear();

  for (vector<KeyFrame*>::const_iterator itKF=mvpLocalKeyFrames.begin(), itEndKF=mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++)
  {
    KeyFrame* pKF = *itKF;
    const vector<MapPoint*> vpMPs = pKF->GetMapPointMatches();

    for (vector<MapPoint*>::const_iterator itMP=vpMPs.begin(), itEndMP=vpMPs.end(); itMP!=itEndMP; itMP++)
    {
      MapPoint* pMP = *itMP;
      if (!pMP)
        continue;
      if (pMP->mnTrackReferenceForFrame==mCurrentFrame.mnId)
        continue;
      if (!pMP->isBad())
      {
        mvpLocalMapPoints.push_back(pMP);
        pMP->mnTrackReferenceForFrame=mCurrentFrame.mnId;
      }
    }
  }
}


void Tracking::UpdateLocalKeyFrames()
{
  // Each map point vote for the keyframes in which it has been observed
  map<KeyFrame*,int> keyframeCounter;
  for (int i=0; i<mCurrentFrame.N; i++)
  {
    if (mCurrentFrame.mvpMapPoints[i])
    {
      MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
      if (!pMP->isBad())
      {
        const map<KeyFrame*,size_t> observations = pMP->GetObservations();
        for (map<KeyFrame*,size_t>::const_iterator it=observations.begin(), itend=observations.end(); it!=itend; it++)
          keyframeCounter[it->first]++;
      }
      else
      {
        mCurrentFrame.mvpMapPoints[i]=NULL;
      }
    }
  }

  if (keyframeCounter.empty())
    return;

  int max=0;
  KeyFrame* pKFmax= static_cast<KeyFrame*>(NULL);

  mvpLocalKeyFrames.clear();
  mvpLocalKeyFrames.reserve(3*keyframeCounter.size());

  // All keyframes that observe a map point are included in the local map. Also check which keyframe shares most points
  for (map<KeyFrame*,int>::const_iterator it=keyframeCounter.begin(), itEnd=keyframeCounter.end(); it!=itEnd; it++)
  {
    KeyFrame* pKF = it->first;

    if (pKF->isBad())
      continue;

    if (it->second>max)
    {
      max=it->second;
      pKFmax=pKF;
    }

    mvpLocalKeyFrames.push_back(it->first);
    pKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
  }


  // Include also some not-already-included keyframes that are neighbors to already-included keyframes
  for (vector<KeyFrame*>::const_iterator itKF=mvpLocalKeyFrames.begin(), itEndKF=mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++)
  {
    // Limit the number of keyframes
    if (mvpLocalKeyFrames.size()>80)
      break;

    KeyFrame* pKF = *itKF;

    const vector<KeyFrame*> vNeighs = pKF->GetBestCovisibilityKeyFrames(10);

    for (vector<KeyFrame*>::const_iterator itNeighKF=vNeighs.begin(), itEndNeighKF=vNeighs.end(); itNeighKF!=itEndNeighKF; itNeighKF++)
    {
      KeyFrame* pNeighKF = *itNeighKF;
      if (!pNeighKF->isBad())
      {
        if (pNeighKF->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
        {
          mvpLocalKeyFrames.push_back(pNeighKF);
          pNeighKF->mnTrackReferenceForFrame=mCurrentFrame.mnId;
          break;
        }
      }
    }

    const set<KeyFrame*> spChilds = pKF->GetChilds();
    for (set<KeyFrame*>::const_iterator sit=spChilds.begin(), send=spChilds.end(); sit!=send; sit++)
    {
      KeyFrame* pChildKF = *sit;
      if (!pChildKF->isBad())
      {
        if (pChildKF->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
        {
          mvpLocalKeyFrames.push_back(pChildKF);
          pChildKF->mnTrackReferenceForFrame=mCurrentFrame.mnId;
          break;
        }
      }
    }

    KeyFrame* pParent = pKF->GetParent();
    if (pParent)
    {
      if (pParent->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
      {
        mvpLocalKeyFrames.push_back(pParent);
        pParent->mnTrackReferenceForFrame=mCurrentFrame.mnId;
        break;
      }
    }

  }

  if (pKFmax)
  {
    mpReferenceKF = pKFmax;
    mCurrentFrame.mpReferenceKF = mpReferenceKF;
  }
}

bool Tracking::Relocalization() {
  ORBmatcher matcher(0.75,true);
  int nmatches, nGood;

  // Compare to all keyframes starting from the last one
  vector<KeyFrame*> kfs = mpMap->GetAllKeyFrames();
  for (auto it=kfs.rbegin(); it != kfs.rend(); it++) {
    KeyFrame* kf = *it;

    mCurrentFrame.SetPose(kf->GetPose());

    // Try to align current frame and candidate keyframe
    ImageAlign image_align;
    if (!image_align.ComputePose(mCurrentFrame, kf, true))
      continue;

    fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));

    // Project points seen in previous frame
    nmatches = matcher.SearchByProjection(mCurrentFrame, kf, threshold_, mSensor==System::MONOCULAR);
    if (nmatches < 20)
      continue;

    // Optimize frame pose with all matches
    nGood = Optimizer::PoseOptimization(&mCurrentFrame);
    if (nGood < 10)
      continue;

    mnLastRelocFrameId = mCurrentFrame.mnId;
    return true;
  }

  return false;
}

void Tracking::Reset() {

  cout << "System Reseting" << endl;
  if (mpViewer) {
    mpViewer->RequestStop();
    while (!mpViewer->isStopped())
      usleep(3000);
  }

  // Reset Local Mapping
  cout << "Reseting Local Mapper...";
  mpLocalMapper->RequestReset();
  cout << " done" << endl;

  // Reset Loop Closing
  cout << "Reseting Loop Closing...";
  mpLoopClosing->RequestReset();
  cout << " done" << endl;

  // Clear Map (this erase MapPoints and KeyFrames)
  mpMap->clear();

  KeyFrame::nNextId = 0;
  Frame::nNextId = 0;
  mState = NO_IMAGES_YET;

  if (mpInitializer)
  {
    delete mpInitializer;
    mpInitializer = static_cast<Initializer*>(NULL);
  }

  mlRelativeFramePoses.clear();
  mlpReferences.clear();
  mlFrameTimes.clear();
  mlbLost.clear();

  if (mpViewer)
    mpViewer->Release();
}

void Tracking::ChangeCalibration(const string &strSettingPath) {
  cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
  float fx = fSettings["Camera.fx"];
  float fy = fSettings["Camera.fy"];
  float cx = fSettings["Camera.cx"];
  float cy = fSettings["Camera.cy"];

  cv::Mat K = cv::Mat::eye(3,3,CV_32F);
  K.at<float>(0,0) = fx;
  K.at<float>(1,1) = fy;
  K.at<float>(0,2) = cx;
  K.at<float>(1,2) = cy;
  K.copyTo(mK);

  cv::Mat DistCoef(4,1,CV_32F);
  DistCoef.at<float>(0) = fSettings["Camera.k1"];
  DistCoef.at<float>(1) = fSettings["Camera.k2"];
  DistCoef.at<float>(2) = fSettings["Camera.p1"];
  DistCoef.at<float>(3) = fSettings["Camera.p2"];
  const float k3 = fSettings["Camera.k3"];
  if (k3!=0)
  {
    DistCoef.resize(5);
    DistCoef.at<float>(4) = k3;
  }
  DistCoef.copyTo(mDistCoef);

  mbf = fSettings["Camera.bf"];

  Frame::mbInitialComputations = true;
}

} //namespace ORB_SLAM
