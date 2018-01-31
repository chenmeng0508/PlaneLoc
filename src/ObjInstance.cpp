/*
    Copyright (c) 2017 Mobile Robots Laboratory at Poznan University of Technology:
    -Jan Wietrzykowski name.surname [at] put.poznan.pl

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include <thread>
#include <chrono>
#include <vector>
#include <list>

//#include <pcl/sample_consensus/model_types.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/surface/convex_hull.h>
#include <pcl/surface/concave_hull.h>
#include <pcl/common/transforms.h>
#include <pcl/common/pca.h>

#include "ObjInstance.hpp"
#include "Misc.hpp"
#include "Exceptions.hpp"
#include "Types.hpp"
#include "Matching.hpp"
#include "UnionFind.h"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/algorithm.h>
#include <CGAL/Delaunay_triangulation_2.h>
#include <CGAL/Alpha_shape_2.h>
#include <CGAL/IO/io.h>
#include <CGAL/Polygon_2.h>
#include <CGAL/Polyline_simplification_2/simplify.h>

using namespace std;

ObjInstance::ObjInstance(int iid,
					ObjType itype,
					pcl::PointCloud<pcl::PointXYZRGB>::Ptr ipoints,
					const std::vector<PlaneSeg>& isvs)
	: id(iid),
	  type(itype),
	  points(ipoints),
	  svs(isvs),
	  convexHull(new pcl::PointCloud<pcl::PointXYZRGB>())
{
    pcl::PCA<pcl::PointXYZRGB> pca;
    pca.setInputCloud(points);
    
    Eigen::Matrix3f evecs = pca.getEigenVectors();
    Eigen::Vector3f evals = pca.getEigenValues();
    Eigen::Vector4f pcaMean = pca.getMean();
    
    Eigen::Vector3f ev0 = evecs.block<3, 1>(0, 0);
    Eigen::Vector3f ev1 = evecs.block<3, 1>(0, 1);
    Eigen::Vector3f ev2 = evecs.block<3, 1>(0, 2);
    
    // shorter side of the plane is the second largest eigenvalue
    shorterComp = sqrt(evals(1)/points->size());
    
    // the eigenvector for the smallest eigenvalue is the normal vector
    paramRep.head<3>() = ev2.cast<double>();
    // distance is the dot product of normal and point lying on the plane
    paramRep(3) = -ev2.dot(pcaMean.head<3>());
    
    princComp = vector<Eigen::Vector3d>{ev0.cast<double>(), ev1.cast<double>(), ev2.cast<double>()};
    princCompLens = vector<double>{evals(0), evals(1), evals(2)};
    
    curv = evals(2) / (evals(0) + evals(1) + evals(2));
    
//	Eigen::Vector4f tmpParamRep;
//	pcl::computePointNormal(*points, tmpParamRep, curv);
    
    bool corrOrient = true;
    int corrCnt = 0;
    int incorrCnt = 0;
    for(int sv = 0; sv < svs.size(); ++sv){
        pcl::PointNormal svPtNormal;
        Eigen::Vector3d svNormal = svs[sv].getSegNormal().cast<double>();
        // if cross product between normal vectors is negative then it is wrongly oriented
        if(svNormal.dot(paramRep.head<3>()) < 0){
            ++incorrCnt;
        }
        else{
            ++corrCnt;
        }
    }
    if(incorrCnt > corrCnt){
        corrOrient = false;
    }
    if(incorrCnt != 0 && corrCnt != 0){
//        throw PLANE_EXCEPTION("Some normals correct and some incorrect");
		cout << "Some normals correct and some incorrect" << endl;
        for(int sv = 0; sv < svs.size(); ++sv) {
            // if cross product between normal vectors is negative then it is wrongly oriented
            Eigen::Vector3f svNormal = svs[sv].getSegNormal();
            cout << "svNormal[" << sv << "] = " << svNormal.transpose() << endl;
        }
    }
    // flip the normal
    if(!corrOrient){
        paramRep = -paramRep;
    }
    // normal including distance from origin
    normal = paramRep;
    
    // normalize paramRep
	Misc::normalizeAndUnify(paramRep);

    hull = ConcaveHull(points, normal);
}

void ObjInstance::transform(Vector7d transform) {
    g2o::SE3Quat transformSE3Quat(transform);
    Eigen::Matrix4d transformMat = transformSE3Quat.to_homogeneous_matrix();
    Eigen::Matrix3d R = transformMat.block<3, 3>(0, 0);
    Eigen::Matrix4d Tinvt = transformMat.inverse();
    Tinvt = Tinvt.transpose();
    
    // pcl::PointCloud<pcl::PointXYZRGB>::Ptr points;
    pcl::transformPointCloud(*points, *points, transformMat);

    // std::vector<PlaneSeg> svs;
    for(PlaneSeg &pseg : svs){
        pseg.transform(transform);
    }

    // Eigen::Vector4d paramRep;
    paramRep = Tinvt * paramRep;
    Misc::normalizeAndUnify(paramRep);
    
    // Eigen::Vector4d normal;
    normal = Tinvt * normal;
    
    // std::vector<Eigen::Vector3d> princComp;
    for(Eigen::Vector3d &pc : princComp){
        pc = R * pc;
    }
    
    // std::vector<double> princCompLens;
    // no need to transform
    
    // double shorterComp;
    // no need to transform
    
    // float curv;
    // no need to transform
    
    // ConcaveHull hull;
    hull = ConcaveHull(points, normal);
    
    // std::vector<LineSeg> lineSegs;
    // TODO
}

std::vector<ObjInstance> ObjInstance::mergeObjInstances(const std::vector<std::vector<ObjInstance>>& objInstances,
                                                        pcl::visualization::PCLVisualizer::Ptr viewer,
                                                        int viewPort1,
                                                        int viewPort2)
{
    static constexpr double shadingLevel = 0.01;

    vector<ObjInstance> retObjInstances;

    if(viewer){
        viewer->removeAllPointClouds(viewPort1);
        viewer->removeAllShapes(viewPort1);
        viewer->removeAllPointClouds(viewPort2);
        viewer->removeAllShapes(viewPort2);

        viewer->initCameraParameters();
        viewer->setCameraPosition(0.0, 0.0, -6.0, 0.0, 1.0, 0.0);
    }

    int planesCnt = 0;
    vector<vector<int> > planeIds;
    for(int ba = 0; ba < objInstances.size(); ++ba){
        planeIds.emplace_back(objInstances[ba].size());
        for(int pl = 0; pl < objInstances[ba].size(); ++pl){
            planeIds[ba][pl] = planesCnt++;
        }
    }
    UnionFind ufSets(planesCnt);
    Vector7d transform;
    // identity
    transform << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0;
    for(int ba = 0; ba < objInstances.size(); ++ba){

        if(viewer){
            viewer->removeAllPointClouds(viewPort1);
            viewer->removeAllShapes(viewPort1);

            for(int pl = 0; pl < objInstances[ba].size(); ++pl){
                const pcl::PointCloud<pcl::PointXYZRGB>::Ptr curPl = objInstances[ba][pl].getPoints();

                viewer->addPointCloud(curPl, string("plane_ba_") + to_string(pl), viewPort1);
                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                         shadingLevel,
                                                         string("plane_ba_") + to_string(pl),
                                                         viewPort1);
//                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
//                                                         0.1, 0.1, 0.1,
//                                                         string("plane_ba_") + to_string(pl),
//                                                         viewPort1);
            }
        }

        for(int pl = 0; pl < objInstances[ba].size(); ++pl){
            const ObjInstance& curObj = objInstances[ba][pl];
            Eigen::Vector3d curObjNormal = curObj.getNormal().head<3>();

            if(viewer){
                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                         0.5,
                                                         string("plane_ba_") + to_string(pl),
                                                         viewPort1);
                
                
                const vector<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> &chullPointClouds =
                            curObj.getHull().getPolygons3d();
                
                for(int poly = 0; poly < chullPointClouds.size(); ++poly) {
                    pcl::Vertices chullVertices;
                    chullVertices.vertices.resize(chullPointClouds[poly]->size());
                    iota(chullVertices.vertices.begin(), chullVertices.vertices.end(), 0);
                    chullVertices.vertices.push_back(chullVertices.vertices.front());
                    
                    pcl::PointCloud<pcl::PointXYZRGB>::Ptr chullPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
                    for (int p = 0; p < chullVertices.vertices.size(); ++p) {
                        chullPoints->push_back(chullPointClouds[poly]->at(chullVertices.vertices[p]));
                    }
                    viewer->addPolygonMesh<pcl::PointXYZRGB>(chullPointClouds[poly],
                                                             vector<pcl::Vertices>{chullVertices},
                                                             string("polygon_ba_") + to_string(pl) +
                                                                   "_" + to_string(poly),
                                                             viewPort1);
                    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
                                                             0,
                                                             1.0,
                                                             0,
                                                             string("polygon_ba_") + to_string(pl) +
                                                                                     "_" + to_string(poly),
                                                             viewPort1);
                    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                             0.5,
                                                             string("polygon_ba_") + to_string(pl) +
                                                                                     "_" + to_string(poly),
                                                             viewPort1);
                    viewer->addPolygon<pcl::PointXYZRGB>(chullPoints,
                                                         1.0,
                                                         0.0,
                                                         0.0,
                                                         string("polyline_ba_") + to_string(pl) +
                                                                                  "_" + to_string(poly),
                                                         viewPort1);
                    viewer->addPointCloud(chullPoints,
                                          string("polygon_pc_ba_") + to_string(pl) +
                                                                     "_" + to_string(poly),
                                          viewPort1);
                    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
                                                             0.0, 0.0, 1.0,
                                                             string("polygon_pc_ba_") +
                                                             to_string(pl) +
                                                             "_" + to_string(poly),
                                                             viewPort1);
                    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE,
                                                             4,
                                                             string("polygon_pc_ba_") +
                                                             to_string(pl) +
                                                             "_" + to_string(poly),
                                                             viewPort1);
                    cout << "polygon for plane (pl) " << pl << ", size = "
                         << chullVertices.vertices.size() << endl;
                }
            }
            for(int cba = ba; cba < objInstances.size(); ++cba){

                if(viewer){
                    viewer->removeAllPointClouds(viewPort2);
                    viewer->removeAllShapes(viewPort2);

                    for(int cpl = 0; cpl < objInstances[cba].size(); ++cpl){
                        const pcl::PointCloud<pcl::PointXYZRGB>::Ptr curPl = objInstances[cba][cpl].getPoints();

                        viewer->addPointCloud(curPl, string("plane_cba_") + to_string(cpl), viewPort2);
                        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                                 shadingLevel,
                                                                 string("plane_cba_") + to_string(cpl),
                                                                 viewPort2);
//                        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
//                                                                 0.1, 0.1, 0.1,
//                                                                 string("plane_cba_") + to_string(cpl),
//                                                                 viewPort2);
                    }
                }

                int startCpl = 0;
                if(cba == ba){
                    startCpl = pl + 1;
                }
                for(int cpl = startCpl; cpl < objInstances[cba].size(); ++cpl){
                    const ObjInstance& compObj = objInstances[cba][cpl];
                    Eigen::Vector3d compObjNormal = compObj.getNormal().head<3>();

                    if(viewer){
                        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                                 0.5,
                                                                 string("plane_cba_") + to_string(cpl),
                                                                 viewPort2);
    
                        const vector<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> &chullPointClouds =
                                compObj.getHull().getPolygons3d();
    
                        for(int poly = 0; poly < chullPointClouds.size(); ++poly) {
                            pcl::Vertices chullVertices;
                            chullVertices.vertices.resize(chullPointClouds[poly]->size());
                            iota(chullVertices.vertices.begin(), chullVertices.vertices.end(), 0);
                            chullVertices.vertices.push_back(chullVertices.vertices.front());
        
                            pcl::PointCloud<pcl::PointXYZRGB>::Ptr chullPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
                            for (int p = 0; p < chullVertices.vertices.size(); ++p) {
                                chullPoints->push_back(chullPointClouds[poly]->at(chullVertices.vertices[p]));
                            }
                            viewer->addPolygonMesh<pcl::PointXYZRGB>(chullPointClouds[poly],
                                                                     vector<pcl::Vertices>{chullVertices},
                                                                     string("polygon_cba_") + to_string(cpl) +
                                                                     "_" + to_string(poly),
                                                                     viewPort1);
                            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
                                                                     0,
                                                                     1.0,
                                                                     0,
                                                                     string("polygon_cba_") + to_string(cpl) +
                                                                     "_" + to_string(poly),
                                                                     viewPort1);
                            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                                     0.5,
                                                                     string("polygon_cba_") + to_string(cpl) +
                                                                     "_" + to_string(poly),
                                                                     viewPort1);
                            viewer->addPolygon<pcl::PointXYZRGB>(chullPoints,
                                                                 1.0,
                                                                 0.0,
                                                                 0.0,
                                                                 string("polyline_cba_") + to_string(cpl) +
                                                                 "_" + to_string(poly),
                                                                 viewPort1);
                            viewer->addPointCloud(chullPoints,
                                                  string("polygon_pc_cba_") + to_string(cpl) +
                                                  "_" + to_string(poly),
                                                  viewPort1);
                            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
                                                                     0.0, 0.0, 1.0,
                                                                     string("polygon_pc_cba_") +
                                                                     to_string(pl) +
                                                                     "_" + to_string(poly),
                                                                     viewPort1);
                            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE,
                                                                     4,
                                                                     string("polygon_pc_cba_") +
                                                                     to_string(cpl) +
                                                                     "_" + to_string(poly),
                                                                     viewPort1);
                            cout << "polygon for plane (pl) " << pl << ", size = "
                                 << chullVertices.vertices.size() << endl;
                        }
//                        for(int p = 1; p < chullPolygon.vertices.size(); ++p){
//                            viewer->addLine(chullPointCloud->at(chullPolygon.vertices[p - 1]),
//                                            chullPointCloud->at(chullPolygon.vertices[p]),
//                                            1.0, 0.0, 0.0,
//                                            "cur_line",
//                                            viewPort2);
//                            viewer->setShapeRenderingProperties(pcl::visualization::PCL_VISUALIZER_LINE_WIDTH,
//                                                                4,
//                                                                "cur_line",
//                                                                viewPort2);
//                            viewer->setShapeRenderingProperties(pcl::visualization::PCL_VISUALIZER_SHADING,
//                                                                pcl::visualization::PCL_VISUALIZER_SHADING_FLAT,
//                                                                "cur_line",
//                                                                viewPort2);
//                            cout << "point " << chullPointCloud->at(chullPolygon.vertices[p]) << endl;
//
//                            viewer->resetStoppedFlag();
//                            while (!viewer->wasStopped()){
//                                viewer->spinOnce (100);
//                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
//                            }
//
//                            viewer->removeShape("cur_line", viewPort2);
//
////                            cout << "point " << chullPolygon.vertices[p] << endl;
//                        }
                    }

                    double diff = Matching::planeEqDiffLogMap(curObj, compObj, transform);
//                    cout << "diff = " << diff << endl;
                    // if plane equation is similar
                    if(diff < 0.01){
                        double normDot = curObjNormal.dot(compObjNormal);
//                        cout << "normDot = " << normDot << endl;
                        // if the observed face is the same
                        if(normDot > 0){
                            double intArea = 0.0;
                            double intScore = Matching::checkConvexHullIntersection(curObj, compObj, transform, intArea);
//                            cout << "intScore = " << intScore << endl;
                            // if intersection of convex hulls is big enough
                            if(intScore > 0.3){
                                cout << "merging planes" << endl;
                                // join the objects
                                ufSets.unionSets(planeIds[ba][pl], planeIds[cba][cpl]);
                            }
                        }
                    }

                    if(viewer){
                        viewer->resetStoppedFlag();

//                        viewer->initCameraParameters();
//                        viewer->setCameraPosition(0.0, 0.0, -6.0, 0.0, 1.0, 0.0);
                        while (!viewer->wasStopped()){
                            viewer->spinOnce (100);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }


                        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                                 shadingLevel,
                                                                 string("plane_cba_") + to_string(cpl),
                                                                 viewPort2);
                        const vector<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> &chullPointClouds =
                                compObj.getHull().getPolygons3d();
    
                        for(int poly = 0; poly < chullPointClouds.size(); ++poly) {
                            viewer->removePolygonMesh(string("polygon_cba_") + to_string(cpl) +
                                                          "_" + to_string(poly),
                                                      viewPort2);
                            viewer->removeShape(string("polyline_cba_") + to_string(cpl) +
                                                    "_" + to_string(poly),
                                                viewPort2);
                            viewer->removePointCloud(string("polygon_pc_cba_") + to_string(cpl) +
                                                        "_" + to_string(poly),
                                                     viewPort2);
                        }
                    }
                }
            }

            if(viewer){
                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
                                                         shadingLevel,
                                                         string("plane_ba_") + to_string(pl),
                                                         viewPort1);
                const vector<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> &chullPointClouds =
                        curObj.getHull().getPolygons3d();
    
                for(int poly = 0; poly < chullPointClouds.size(); ++poly) {
                    viewer->removePolygonMesh(string("polygon_ba_") + to_string(pl) +
                                                 "_" + to_string(poly),
                                              viewPort1);
                    viewer->removeShape(string("polyline_ba_") + to_string(pl) +
                                           "_" + to_string(poly),
                                        viewPort1);
                    viewer->removePointCloud(string("polygon_pc_ba_") + to_string(pl) +
                                                "_" + to_string(poly),
                                             viewPort1);
                }
            }
        }
    }

    multimap<int, pair<int, int> > sets;
    for(int ba = 0; ba < objInstances.size(); ++ba) {
        for (int pl = 0; pl < objInstances[ba].size(); ++pl) {
            int setId = ufSets.findSet(planeIds[ba][pl]);
            sets.insert(make_pair(setId, make_pair(ba, pl)));
        }
    }
    if(viewer){
        viewer->removeAllPointClouds();
        viewer->removeAllShapes();
    }
//    typedef multimap<int, pair<int, int> >::iterator mmIter;
    for(auto it = sets.begin(); it != sets.end(); ){
        auto range = sets.equal_range(it->first);

        vector<const ObjInstance*> curObjs;
        for(auto rangeIt = range.first; rangeIt != range.second; ++rangeIt){
            curObjs.emplace_back(&(objInstances[rangeIt->second.first][rangeIt->second.second]));
        }
        if(curObjs.size() == 1){
            retObjInstances.push_back(*curObjs.front());
        }
        else{
            retObjInstances.push_back(merge(curObjs,
                                            viewer,
                                            viewPort1,
                                            viewPort2));
        }
        
        it = range.second;
    
        if(viewer) {
            const pcl::PointCloud<pcl::PointXYZRGB>::Ptr curPl = retObjInstances.back().getPoints();
            viewer->addPointCloud(curPl, string("plane_ret_") + to_string(it->first), viewPort1);
        }
    }

    return retObjInstances;
}

ObjInstance ObjInstance::merge(const std::vector<const ObjInstance*>& objInstances,
                               pcl::visualization::PCLVisualizer::Ptr viewer,
                               int viewPort1,
                               int viewPort2)
{
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr newPoints(new pcl::PointCloud<pcl::PointXYZRGB>());
    std::vector<PlaneSeg> newSvs;

    Eigen::Vector3d meanLogMap;
    meanLogMap << 0.0, 0.0, 0.0;
    int sumPoints = 0;
    for(int o = 0; o < objInstances.size(); ++o){
        Eigen::Vector4d curParamRep = objInstances[o]->getParamRep();
        Eigen::Vector3d logMapParamRep = Misc::logMap(Eigen::Quaterniond(curParamRep));
        meanLogMap += logMapParamRep * objInstances[o]->getPoints()->size();
        sumPoints += objInstances[o]->getPoints()->size();
    }
    meanLogMap /= sumPoints;

    Eigen::Quaterniond meanParamRep = Misc::expMap(meanLogMap);
    Eigen::Vector4d meanPlaneEq;
    meanPlaneEq[0] = meanParamRep.x();
    meanPlaneEq[1] = meanParamRep.y();
    meanPlaneEq[2] = meanParamRep.z();
    meanPlaneEq[3] = meanParamRep.w();
    double normNorm = meanPlaneEq.head<3>().norm();
    meanPlaneEq.head<3>() /= normNorm;
    meanPlaneEq[3] /= normNorm;

    pcl::ModelCoefficients::Ptr mdlCoeff (new pcl::ModelCoefficients);
    mdlCoeff->values.resize(4);
    mdlCoeff->values[0] = meanPlaneEq[0];
    mdlCoeff->values[1] = meanPlaneEq[1];
    mdlCoeff->values[2] = meanPlaneEq[2];
    mdlCoeff->values[3] = meanPlaneEq[3];
    for(int o = 0; o < objInstances.size(); ++o){
        pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointsProj(new pcl::PointCloud<pcl::PointXYZRGB>);
        pcl::ProjectInliers<pcl::PointXYZRGB> proj;
        proj.setModelType(pcl::SACMODEL_PLANE);
        proj.setInputCloud(objInstances[o]->getPoints());
        proj.setModelCoefficients(mdlCoeff);
        proj.filter(*pointsProj);

        const vector<PlaneSeg>& svs = objInstances[o]->getSvs();

        newPoints->insert(newPoints->end(), pointsProj->begin(), pointsProj->end());
        newSvs.insert(newSvs.end(), svs.begin(), svs.end());

        if(viewer) {
            viewer->addPointCloud(objInstances[o]->getPoints(), string("plane_o_") + to_string(o), viewPort1);

        }
    }

    if(viewer){
        viewer->addPointCloud(newPoints, string("plane_merged"), viewPort2);
        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR,
                                                 1.0, 0.0, 0.0,
                                                 string("plane_merged"),
                                                 viewPort2);

        viewer->resetStoppedFlag();

//                        viewer->initCameraParameters();
//                        viewer->setCameraPosition(0.0, 0.0, -6.0, 0.0, 1.0, 0.0);
        while (!viewer->wasStopped()){
            viewer->spinOnce (100);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        for(int o = 0; o < objInstances.size(); ++o){
            viewer->removePointCloud(string("plane_o_") + to_string(o), viewPort1);
        }
        viewer->removePointCloud(string("plane_merged"), viewPort2);
    }

    return ObjInstance(0,
                        ObjInstance::ObjType::Plane,
                        newPoints,
                        newSvs);
}


