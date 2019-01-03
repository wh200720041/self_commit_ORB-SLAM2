/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Initializer.h"

#include "Thirdparty/DBoW2/DUtils/Random.h"

#include "Optimizer.h"
#include "ORBmatcher.h"

#include<thread>

namespace ORB_SLAM2
{

/**
 * @brief 给定参考帧构造Initializer
 * 
 * 用reference frame来初始化，这个reference frame就是SLAM正式开始的第一帧
 * @param ReferenceFrame I	参考帧
 * @param sigma          I	TODO 测量误差,但是我到现在也还是不知道这个是不是外部给定的，又是谁给定的
 * @param iterations     I	RANSAC迭代次数，外部给定
 */
Initializer::Initializer(const Frame &ReferenceFrame, float sigma, int iterations)
{
	//获取相机的内参数矩阵
    mK = ReferenceFrame.mK.clone();
	
	//获取保存参考帧特征点的向量
    mvKeys1 = ReferenceFrame.mvKeysUn;
	
	//获取估计误差
    mSigma = sigma;
	//并计算其平方
    mSigma2 = sigma*sigma;
	//保存最大迭代次数
    mMaxIterations = iterations;
}

/**
 * @brief 并行地计算基础矩阵和单应性矩阵，选取其中一个来恢复出最开始两帧之间的相对姿态，并进行三角化测量得到最初两帧的点云
 * @param CurrentFrame 		I	当前帧，也就是SLAM意义上的第二帧
 * @param vMatches12 		I	当前帧和参考帧图像中特征点的匹配关系
 * @param R21				O	相机从参考帧到当前帧所发生的旋转
 * @param t21				O	相机从参考帧到当前帧所发生的平移
 * @param vP3D				O	三角化测量之后的特征点的空间坐标
 * @param vbTriangulated	O	基于特征点匹配关系，标记某对特征点是否被三角化测量
 * @return 初始化是否成功
 */
bool Initializer::Initialize(const Frame &CurrentFrame, const vector<int> &vMatches12, cv::Mat &R21, cv::Mat &t21,
                             vector<cv::Point3f> &vP3D, vector<bool> &vbTriangulated)
{
    // Fill structures with current keypoints and matches with reference frame
    // Reference Frame: 1, Current Frame: 2
    // Frame2 特征点
    mvKeys2 = CurrentFrame.mvKeysUn;

    // mvMatches12记录匹配上的特征点对
    mvMatches12.clear();
	// 预分配空间
	//分配mvKeys2.size()这么多的空间的原因是，后面的for操作是按照vMatches12这个向量来遍历的，而根据下文对程序代码的猜测，这个向量的长度
    //应该是和mvKeys2相同的。由于这个只保存参考帧和当前帧的特征点对应关系，所以不一定最后就能够有mvKeys2.size()个匹配关系，因此使用的是
    //std::vector::reserve()这个函数
    mvMatches12.reserve(mvKeys2.size());
    // mvbMatched1记录参考帧中的每个特征点是否有匹配的特征点，
    //这个成员变量后面没有用到，后面只关心匹配上的特征点 	
    mvbMatched1.resize(mvKeys1.size());

    // 步骤1：组织特征点对
    for(size_t i=0, iend=vMatches12.size();i<iend; i++)
    {
		//TODO 猜测，这里vMatches12的下标是参考帧中特征点在其向量中的下标，其内容则是对应的当前帧中特征点在其向量中的下标
        if(vMatches12[i]>=0)
        {
			//所以，保存这个匹配关系，实际上就是保存了特征点在各自的向量中的下标
            mvMatches12.push_back(make_pair(i,vMatches12[i]));
			//并且标记参考帧中的这个特征点有匹配关系
            mvbMatched1[i]=true;
        }
        else
			//否则，就没有
            mvbMatched1[i]=false;
    }

    // 匹配上的特征点的对数
    const int N = mvMatches12.size();

    // Indices for minimum set selection
    // 新建一个容器vAllIndices，生成0到N-1的数作为特征点的索引
	//所有特征点对的索引
    vector<size_t> vAllIndices;
	//预分配空间
    vAllIndices.reserve(N);
	//在RANSAC的某次迭代中，还可以被抽取来作为数据样本的特征点对的索引，所以这里起的名字叫做可用的索引
    vector<size_t> vAvailableIndices;

	//初始化所有特征点对的索引
    for(int i=0; i<N; i++)
    {
        vAllIndices.push_back(i);
    }

    // Generate sets of 8 points for each RANSAC iteration
    // 步骤2：在所有匹配特征点对中随机选择8对匹配特征点为一组，共选择mMaxIterations组
    // 用于FindHomography和FindFundamental求解
    // mMaxIterations:200（默认值）
    //这个变量用于保存每次迭代时所使用的向量which was used to 保存八对点 to 进行单应矩阵和基础矩阵估计
    mvSets = vector< vector<size_t> >(mMaxIterations,		//最大的RANSAC迭代次数
									  vector<size_t>(8,0));	//这个则是第二维元素的初始值，也就是第一维。这里其实也是一个第一维的构造函数，第一维vector有8项，每项的初始值为0.

	//用于进行随机数据样本采样，设置随机数种子
    DUtils::Random::SeedRandOnce(0);

	//开始每一次的迭代 
    for(int it=0; it<mMaxIterations; it++)
    {
		//迭代开始的时候，所有的点都是可用的
        vAvailableIndices = vAllIndices;

        // Select a minimum set
		//选择最小的数据样本集，这里我们希望求出相机的位姿，因此最少需要八个点，所以这里就循环了八次
        for(size_t j=0; j<8; j++)
        {
            // 产生0到N-1的随机数，对应的是要抽取的特征点对的索引
            int randi = DUtils::Random::RandomInt(0,vAvailableIndices.size()-1);
            // idx表示哪一个索引对应的特征点对被选中
            int idx = vAvailableIndices[randi];
			
			//将本次迭代这个选中的第j个特征点对的索引添加到mvSets中
            mvSets[it][j] = idx;

            // randi对应的索引已经被选过了，从容器中删除
            // randi对应的索引用最后一个元素替换，并删掉最后一个元素
			// 所以首先把已经抽走的点的索引，替换为vector尾的索引
            vAvailableIndices[randi] = vAvailableIndices.back();
			//然后删除尾
			vAvailableIndices.pop_back();
        }//依次提取出8个特征点对
    }//迭代mMaxIterations次，选取各自迭代时需要用到的最小数据集

    // Launch threads to compute in parallel a fundamental matrix and a homography
    // 步骤3：调用多线程分别用于计算fundamental matrix和homography
    //这两个变量用于标记在H和F的计算中哪些特征点对被认为是Inlier
    vector<bool> vbMatchesInliersH, vbMatchesInliersF;
	//计算出来的单应矩阵和基础矩阵的RANSAC评分，这里其实是采用重投影误差来计算的
    float SH, SF; // score for H and F
    //这两个是经过RANSAC算法后计算出来的单应矩阵和基础矩阵
    cv::Mat H, F; // H and F

    // ref是引用的功能:http://en.cppreference.com/w/cpp/utility/functional/ref
    // 计算homograpy并打分
    thread threadH(&Initializer::FindHomography,	//该线程的主函数
				   this,							//由于主函数为类的成员函数，所以第一个参数就应该是当前对象的this指针
				   ref(vbMatchesInliersH), 			//输出，特征点对的Inlier标记
				   ref(SH), 						//输出，计算的单应矩阵的RANSAC评分
				   ref(H));							//输出，计算的单应矩阵结果
    // 计算fundamental matrix并打分，参数定义是一样的，这里不再赘述
    thread threadF(&Initializer::FindFundamental,this,ref(vbMatchesInliersF), ref(SF), ref(F));

    // Wait until both threads have finished
	//等待两个计算线程结束
    threadH.join();
    threadF.join();

    // Compute ratio of scores
    // 步骤4：计算得分比例，选取某个模型
	//通过这个规则来判断谁的评分占比更多一些，注意不是简单的评分大，而是看评分的占比
    float RH = SH/(SH+SF);			//RH=Ratio of Homography?

    // Try to reconstruct from homography or fundamental depending on the ratio (0.40-0.45)
    // 步骤5：从H矩阵或F矩阵中恢复R,t
    if(RH>0.40)
		//此时从单应矩阵恢复，函数ReconstructH返回bool型结果
        return ReconstructH(vbMatchesInliersH,	//输入，匹配成功的特征点对Inliers标记
							H,					//输入，前面RANSAC计算后的单应矩阵
							mK,					//输入，相机的内参数矩阵
							R21,t21,			//输出，计算出来的相机从参考帧到当前帧所发生的旋转变换和位移变换
							vP3D,				//特征点对经过三角测量之后的空间坐标
							vbTriangulated,		//特征点对是否被三角化的标记
							1.0,				//这个对应的形参为minParallax，即认为某对特征点的三角化测量中，认为其测量有效时
												//需要满足的最小视差角（如果视差角过小则会引起非常大的观测误差）
												//并且这个角度的单位还是弧度 
							50);				//为了进行运动恢复，所需要的最少的三角化测量成功的点个数
    else //if(pF_HF>0.6)
		//NOTICE 注意这里有一段注释掉的代码 作者原来的想法是，必须两个矩阵的评分都要“足够优秀”才会分别选取，如果这个比例在0.5附近，
		//说明使用两种矩阵进行相机运动恢复的效果差不多，所以……那个时候干脆就不恢复了
		//此时从基础矩阵恢复
        return ReconstructF(vbMatchesInliersF,F,mK,R21,t21,vP3D,vbTriangulated,1.0,50);

	//一般地程序不应该执行到这里，如果执行到这里说明程序跑飞了
	//其实也有可能是前面注释掉的代码的补充
    return false;
}

/**
 * @brief 计算单应矩阵，假设场景为平面情况下通过前两帧求取Homography矩阵(current frame 2 到 reference frame 1),并得到该模型的评分
 * @param vbMatchesInliers	O	匹配的特征点对属于inliers的标记
 * @param score				O 	这个单应矩阵的RANSAC评分
 * @param H21				O	单应矩阵计算结果
 */
void Initializer::FindHomography(vector<bool> &vbMatchesInliers, float &score, cv::Mat &H21)
{
    // Number of putative matches
	//匹配的特征点对总数
    const int N = mvMatches12.size();

    // Normalize coordinates
    // 将mvKeys1和mvKey2归一化到均值为0，一阶绝对矩为1，归一化矩阵分别为T1、T2
	//这里所谓的一阶绝对矩其实就是随机变量到取值的中心的绝对值的平均值
	//而特征点坐标向量乘某矩阵可以得到归一化后的坐标向量，那么这个矩阵称之为归一化矩阵，它表示了点集在进行归一化的时候
	//所进行的平移变换和旋转变换
	//归一化后的参考帧和当前帧中的特征点坐标
    vector<cv::Point2f> vPn1, vPn2;
	//各自的归一化矩阵
	//NOTE 其实这里的矩阵归一化操作主要是为了在单目初始化过程中，固定场景的尺度，原理可以参考SLAM十四讲P152
    cv::Mat T1, T2;
    Normalize(mvKeys1,vPn1, T1);
    Normalize(mvKeys2,vPn2, T2);
	//这里求的逆在后面的代码中要用到，辅助进行原始尺度的恢复
    cv::Mat T2inv = T2.inv();

    // Best Results variables
    // F:最终最佳的MatchesInliers与得分
	//历史最佳评分
    score = 0.0;
	//历史最佳评分所对应的特征点对的inliers标记
    vbMatchesInliers = vector<bool>(N,false);

    // Iteration variables
	//迭代过程中使用到的变量
	//某次迭代中，参考帧的特征点坐标
    vector<cv::Point2f> vPn1i(8);
	//某次迭代中，当前帧的特征点坐标
    vector<cv::Point2f> vPn2i(8);
	//以及计算出来的单应矩阵、以及其逆
    cv::Mat H21i, H12i;
    // 每次RANSAC的MatchesInliers与得分
    vector<bool> vbCurrentInliers(N,false);
    float currentScore;

    // Perform all RANSAC iterations and save the solution with highest score
	//下面进行每次的RANSAC迭代
    for(int it=0; it<mMaxIterations; it++)
    {
        // Select a minimum set
		//每次迭代首先需要做的还是生成最小数据集
        for(size_t j=0; j<8; j++)
        {
			//从mvSets中获取当前次迭代的某个特征点对的索引信息
            int idx = mvSets[it][j];

            // vPn1i和vPn2i为匹配的特征点对的坐标
			//首先根据这个特征点对的索引信息分别找到两个特征点在各自图像特征点向量中的索引，然后读取其归一化之后的特征点坐标
            vPn1i[j] = vPn1[mvMatches12[idx].first];
            vPn2i[j] = vPn2[mvMatches12[idx].second];
        }//读取8对特征点的归一化之后的坐标

		//八点法计算单应矩阵
        cv::Mat Hn = ComputeH21(vPn1i,vPn2i);
        // NOTICE 恢复原始的均值和尺度
		//关于这里为什么之前要对特征点进行归一化，后面又恢复这个矩阵的尺度，可以在《计算机视觉中的多视图几何》这本书中找到
		//答案：P193页中讲到的归一化8点算法
        H21i = T2inv*Hn*T1;
		//然后计算逆
        H12i = H21i.inv();

        // 利用重投影误差为当次RANSAC的结果评分
        currentScore = CheckHomography(H21i, H12i, 			//输入，单应矩阵的计算结果
									   vbCurrentInliers, 	//输出，特征点对的Inliers标记
									   mSigma);				//TODO  测量误差，在Initializer类对象构造的时候，由外部给定的

        // 得到最优的vbMatchesInliers与score
        if(currentScore>score)
        {
			//如果当前的计算结果是历史最高，那么就要保存计算结果
            H21 = H21i.clone();
			//保存匹配好的特征点对的Inliers标记
            vbMatchesInliers = vbCurrentInliers;
			//更新历史最优评分
            score = currentScore;
        }//得到最优的计算结果
    }//进行指定次数的RANSAC迭代
}//计算单应矩阵（函数）

/**
 * @brief 计算基础矩阵，假设场景为非平面情况下通过前两帧求取Fundamental矩阵(current frame 2 到 reference frame 1),并得到该模型的评分
 * @param vbMatchesInliers	O	匹配好的特征点对的Inliers标记
 * @param score				O	当前计算好的基础矩阵的RANSAC评分
 * @param F21				O	基础矩阵的计算结果
 */
void Initializer::FindFundamental(vector<bool> &vbMatchesInliers, float &score, cv::Mat &F21)
{
    // Number of putative matches
	// 获取总的匹配好的特征点对数
    const int N = vbMatchesInliers.size();

    // Normalize coordinates
	//归一化坐标
    vector<cv::Point2f> vPn1, vPn2;
    cv::Mat T1, T2;
    Normalize(mvKeys1,vPn1, T1);
    Normalize(mvKeys2,vPn2, T2);
	// NOTICE 注意这里取的是归一化矩阵T2的转置,因为基础矩阵的定义和单应矩阵不同，两者去归一化的计算也不相同
    cv::Mat T2t = T2.t();

    // Best Results variables
	//最优结果
    score = 0.0;
    vbMatchesInliers = vector<bool>(N,false);

    // Iteration variables
	//迭代过程中使用到的变量
    vector<cv::Point2f> vPn1i(8);
    vector<cv::Point2f> vPn2i(8);
    cv::Mat F21i;
    vector<bool> vbCurrentInliers(N,false);
    float currentScore;

    // Perform all RANSAC iterations and save the solution with highest score
	//开始RANSAC迭代
    for(int it=0; it<mMaxIterations; it++)
    {
        // Select a minimum set
		//选取最小数据集
        for(int j=0; j<8; j++)
        {
            int idx = mvSets[it][j];

            vPn1i[j] = vPn1[mvMatches12[idx].first];
            vPn2i[j] = vPn2[mvMatches12[idx].second];
        }//选取最小数据集

        //根据最小数据集，计算基础矩阵
        cv::Mat Fn = ComputeF21(vPn1i,vPn2i);
        F21i = T2t*Fn*T1;

        // 利用重投影误差为当次RANSAC的结果评分
        currentScore = CheckFundamental(F21i, vbCurrentInliers, mSigma);

		//得到最优结果
        if(currentScore>score)
        {
            F21 = F21i.clone();
            vbMatchesInliers = vbCurrentInliers;
            score = currentScore;
        }//得到最优的计算结果
    }//进行RANSAC迭代 
}//计算基础矩阵（函数）

// |x'|     | h1 h2 h3 ||x|
// |y'| = a | h4 h5 h6 ||y|  简写: x' = a H x, a为一个尺度因子
// |1 |     | h7 h8 h9 ||1|
// 使用DLT(direct linear tranform)求解该模型
// x' = a H x 
// ---> (x') 叉乘 (H x)  = 0
// ---> Ah = 0
// A = | 0  0  0 -x -y -1 xy' yy' y'|  h = | h1 h2 h3 h4 h5 h6 h7 h8 h9 |
//     |-x -y -1  0  0  0 xx' yx' x'|
// 通过SVD求解Ah = 0，A^T*A最小特征值对应的特征向量即为解
// 其实也就是右奇异值矩阵的最后一列

/**
 * @brief 从特征点匹配求homography（normalized DLT），其实这里最少用四对点就能够求出来，不过这里为了和基础矩阵统一还是使用了8对点求最小二乘解
 * @param  vP1 	I	归一化后的点, in reference frame
 * @param  vP2 	I	归一化后的点, in current frame
 * @return     单应矩阵
 * @see        Multiple View Geometry in Computer Vision - Algorithm 4.2 p109
 */
cv::Mat Initializer::ComputeH21(const vector<cv::Point2f> &vP1, const vector<cv::Point2f> &vP2)
{
	//获取参与计算的特征点的数目
    const int N = vP1.size();

	//构造用于计算的矩阵A
    cv::Mat A(2*N,				//行，注意每一个点的数据对应两行
			  9,				//列
			  CV_32F); // 2N*9	//数据类型

	//开始构造矩阵A，对于每一个点
    for(int i=0; i<N; i++)
    {
		//获取特征点对的像素坐标
        const float u1 = vP1[i].x;
        const float v1 = vP1[i].y;
        const float u2 = vP2[i].x;
        const float v2 = vP2[i].y;

		//生成这个点的第一行
        A.at<float>(2*i,0) = 0.0;
        A.at<float>(2*i,1) = 0.0;
        A.at<float>(2*i,2) = 0.0;
        A.at<float>(2*i,3) = -u1;
        A.at<float>(2*i,4) = -v1;
        A.at<float>(2*i,5) = -1;
        A.at<float>(2*i,6) = v2*u1;
        A.at<float>(2*i,7) = v2*v1;
        A.at<float>(2*i,8) = v2;

		//生成这个点的第二行
        A.at<float>(2*i+1,0) = u1;
        A.at<float>(2*i+1,1) = v1;
        A.at<float>(2*i+1,2) = 1;
        A.at<float>(2*i+1,3) = 0.0;
        A.at<float>(2*i+1,4) = 0.0;
        A.at<float>(2*i+1,5) = 0.0;
        A.at<float>(2*i+1,6) = -u2*u1;
        A.at<float>(2*i+1,7) = -u2*v1;
        A.at<float>(2*i+1,8) = -u2;

    }//对于每一个点构造矩阵A

    //保存计算结果，vt中的t表示是转置
    cv::Mat u,w,vt;

	//opencv提供的进行奇异值分解的函数
    cv::SVDecomp(A,							//输入，待进行奇异值分解的矩阵
				 w,							//输出，奇异值矩阵
				 u,							//输出，矩阵U
				 vt,						//输出，矩阵V^T
				 cv::SVD::MODIFY_A | 		//输入，MODIFY_A是指允许计算函数可以修改待分解的矩阵，官方文档上说这样可以加快计算速度、节省内存
				     cv::SVD::FULL_UV);		//FULL_UV=把U和VT补充成单位正交方阵

	//注意前面说的是右奇异值矩阵的最后一列，但是在这里因为是vt，转置后了，所以是行；由于A有9列数据，故最后一列的下标为8
	//
    return vt.row(8).reshape(0, 			//转换后的通道数，这里设置为0表示是与前面相同
							 3); 			//转换后的行数
							// v的最后一列
}//计算单应矩阵H（函数）

// x'Fx = 0 整理可得：Af = 0
// A = | x'x x'y x' y'x y'y y' x y 1 |, f = | f1 f2 f3 f4 f5 f6 f7 f8 f9 |
// 通过SVD求解Af = 0，A'A最小特征值对应的特征向量即为解
//其实也是运用DLT解法，由于点是三维的因此使用DLT法构造出来的A矩阵和前面的都有些不一样；但是其他的操作和上面计算单应矩阵
//都是基本相似的

/**
 * @brief 从特征点匹配求fundamental matrix（normalized 8点法）
 * @param  vP1 	I	归一化后的点, in reference frame
 * @param  vP2 	I	归一化后的点, in current frame
 * @return     基础矩阵
 * @see        Multiple View Geometry in Computer Vision - Algorithm 11.1 p282 (中文版 p191)
 */
cv::Mat Initializer::ComputeF21(const vector<cv::Point2f> &vP1,const vector<cv::Point2f> &vP2)
{
	//获取参与计算的特征点对数
    const int N = vP1.size();

	//初始化A矩阵
    cv::Mat A(N,9,CV_32F); // N*9

	//对于每对特征点，生成A矩阵的内容
    for(int i=0; i<N; i++)
    {
        const float u1 = vP1[i].x;
        const float v1 = vP1[i].y;
        const float u2 = vP2[i].x;
        const float v2 = vP2[i].y;

        A.at<float>(i,0) = u2*u1;
        A.at<float>(i,1) = u2*v1;
        A.at<float>(i,2) = u2;
        A.at<float>(i,3) = v2*u1;
        A.at<float>(i,4) = v2*v1;
        A.at<float>(i,5) = v2;
        A.at<float>(i,6) = u1;
        A.at<float>(i,7) = v1;
        A.at<float>(i,8) = 1;
    }

    //存储奇异值分解结果的变量
    cv::Mat u,w,vt;

	//进行奇异值分解
    cv::SVDecomp(A,w,u,vt,cv::SVD::MODIFY_A | cv::SVD::FULL_UV);

	//转换成基础矩阵的形式
    cv::Mat Fpre = vt.row(8).reshape(0, 3); // v的最后一列

	//对初步得来的基础矩阵继续进行一次奇异值分解
    cv::SVDecomp(Fpre,w,u,vt,cv::SVD::MODIFY_A | cv::SVD::FULL_UV);

	//NOTICE 注意基础矩阵的定义中，由于乘了一个t^，这个矩阵因为是从三维向量拓展出来的因此它的秩为2；
	//所以正确的基础矩阵的秩应当小于等于2，这里就是进行的这个处理
    w.at<float>(2)=0; // 秩2约束，将第3个奇异值设为0

	//重新组合好满足秩约束的基础矩阵，作为最终计算结果返回
    return  u*cv::Mat::diag(w)*vt;
}//计算基础矩阵

/**
 * @brief 对给定的homography matrix打分
 * @param H21				I	从参考帧到当前帧的单应矩阵
 * @param H12				I	从当前帧到参考帧的单应矩阵
 * @param vbMatchesInliers	O	匹配好的特征点对的Inliers标记
 * @param sigma				I	估计误差
 * @see
 * - Author's paper - IV. AUTOMATIC MAP INITIALIZATION （2）
 * - Multiple View Geometry in Computer Vision - symmetric transfer errors: 4.2.2 Geometric distance
 * - Multiple View Geometry in Computer Vision - model selection 4.7.1 RANSAC
 */
float Initializer::CheckHomography(const cv::Mat &H21, const cv::Mat &H12, vector<bool> &vbMatchesInliers, float sigma)
{
	//获取特征点对的总大小
    const int N = mvMatches12.size();

	//获取从参考帧到当前帧的单应矩阵的各个元素
    // |h11 h12 h13|
    // |h21 h22 h23|
    // |h31 h32 h33|
    const float h11 = H21.at<float>(0,0);
    const float h12 = H21.at<float>(0,1);
    const float h13 = H21.at<float>(0,2);
    const float h21 = H21.at<float>(1,0);
    const float h22 = H21.at<float>(1,1);
    const float h23 = H21.at<float>(1,2);
    const float h31 = H21.at<float>(2,0);
    const float h32 = H21.at<float>(2,1);
    const float h33 = H21.at<float>(2,2);

	//然后获取它的逆的各个元素
    // |h11inv h12inv h13inv|
    // |h21inv h22inv h23inv|
    // |h31inv h32inv h33inv|
    const float h11inv = H12.at<float>(0,0);
    const float h12inv = H12.at<float>(0,1);
    const float h13inv = H12.at<float>(0,2);
    const float h21inv = H12.at<float>(1,0);
    const float h22inv = H12.at<float>(1,1);
    const float h23inv = H12.at<float>(1,2);
    const float h31inv = H12.at<float>(2,0);
    const float h32inv = H12.at<float>(2,1);
    const float h33inv = H12.at<float>(2,2);

	//给特征点对的Inliers标记预分配空间
    vbMatchesInliers.resize(N);

	//初始化RANSAC评分
    float score = 0;

    // 基于卡方检验计算出的阈值（假设测量有一个像素的偏差）
	// 自由度为2的卡方分布，误差项有95%的概率不符合正态分布时的阈值
    const float th = 5.991;

    //信息矩阵，方差平方的倒数
	//TODO 还不明白为什么泡泡机器人给出的注释说下面的这个是信息矩阵
	//NOTE 不是有一个类成员变量mSigma2吗。。。为什么不直接用那个呢——我猜是程序员忘记了
    const float invSigmaSquare = 1.0/(sigma*sigma);

    // N对特征匹配点
    for(int i=0; i<N; i++)
    {
		//一开始都默认为Inlier
        bool bIn = true;

		//根据索引获取这一对特征点
        const cv::KeyPoint &kp1 = mvKeys1[mvMatches12[i].first];
        const cv::KeyPoint &kp2 = mvKeys2[mvMatches12[i].second];

		//提取特征点的坐标
        const float u1 = kp1.pt.x;
        const float v1 = kp1.pt.y;
        const float u2 = kp2.pt.x;
        const float v2 = kp2.pt.y;

        // Reprojection error in first image
        // x2in1 = H12*x2
        // 将图像2中的特征点单应到图像1中
        // |u1|   |h11inv h12inv h13inv||u2|
        // |v1| = |h21inv h22inv h23inv||v2|
        // |1 |   |h31inv h32inv h33inv||1 |
		//这个是一个归一化系数，因为我们希望单应到图像1中的这个点的齐次坐标，最后一维为1
        const float w2in1inv = 1.0/(h31inv*u2+h32inv*v2+h33inv);	//为了计算方便加了一个倒数
		//计算两个坐标
        const float u2in1 = (h11inv*u2+h12inv*v2+h13inv)*w2in1inv;	//u2_in_image_1
        const float v2in1 = (h21inv*u2+h22inv*v2+h23inv)*w2in1inv;	//v2_in_image_1

        // 计算重投影误差
        const float squareDist1 = (u1-u2in1)*(u1-u2in1)+(v1-v2in1)*(v1-v2in1);

        // 根据方差归一化误差
		//这里的误差只有归一化之后，后面的RANSAC评分在不同点和不同的矩阵（主要是这个）中的比较才会有意义
        const float chiSquare1 = squareDist1*invSigmaSquare;

		//如果这个点的归一化后的重投影误差超过了给定的阈值
		//其实就是如果这个误差的平方和超过了阈值后说明观测的点的误差有95%的概率不符合正态分布
        if(chiSquare1>th)
			//那么说明就是Outliers
            bIn = false;
        else
			//在阈值内才算是Inliers，保持默认设置不变；然后累计对当前使用的单应矩阵的RANSAC评分
			//因为在卡方检验中，那个和越小说明数据越符合正态分布，因此用th-来取反，这样数据越好score越大
            score += th - chiSquare1;
		
		//为了使这个计算能够比较好地反应矩阵的实际计算效果，因此这里还要再反方向进行一次重投影误差的计算
        // Reprojection error in second image
        // x1in2 = H21*x1
        // 将图像1中的特征点单应到图像2中
		//缩放因子
        const float w1in2inv = 1.0/(h31*u1+h32*v1+h33);
		//两个坐标
        const float u1in2 = (h11*u1+h12*v1+h13)*w1in2inv;
        const float v1in2 = (h21*u1+h22*v1+h23)*w1in2inv;

		//计算重投影误差
        const float squareDist2 = (u2-u1in2)*(u2-u1in2)+(v2-v1in2)*(v2-v1in2);

		//重投影误差的归一化
        const float chiSquare2 = squareDist2*invSigmaSquare;

		//比较归一化后的重投影误差是否大于阈值
        if(chiSquare2>th)
			//大于阈值说明是Outlier
            bIn = false;
        else
			//反之则是Inlier，保持标志不变；同时累计评分
            score += th - chiSquare2;

		//注意，只有两个点都是Inlier才会认为这对匹配关系是Inlier;
		//只要有一个特征点是Outlier那么就认为这对特征点的匹配关系是Outlier
        if(bIn)
            vbMatchesInliers[i]=true;
        else
            vbMatchesInliers[i]=false;
    }//对于每对匹配好的特征点

    //返回当前给出的单应矩阵的评分
    return score;
}//计算给出的单应矩阵的RANSAC评分

/**
 * @brief 对给定的fundamental matrix打分
 * @param F21				I	从当前帧到参考帧的基础矩阵
 * @param vbMatchesInliers	O	匹配的特征点对属于inliers的标记
 * @param sigma				I	TODO 估计误差？
 * @see
 * - Author's paper - IV. AUTOMATIC MAP INITIALIZATION （2）
 * - Multiple View Geometry in Computer Vision - symmetric transfer errors: 4.2.2 Geometric distance
 * - Multiple View Geometry in Computer Vision - model selection 4.7.1 RANSAC
 */
float Initializer::CheckFundamental(const cv::Mat &F21, vector<bool> &vbMatchesInliers, float sigma)
{
	//获取匹配的特征点对的总对数
    const int N = mvMatches12.size();

	//然后提取基础矩阵中的元素数据
    const float f11 = F21.at<float>(0,0);
    const float f12 = F21.at<float>(0,1);
    const float f13 = F21.at<float>(0,2);
    const float f21 = F21.at<float>(1,0);
    const float f22 = F21.at<float>(1,1);
    const float f23 = F21.at<float>(1,2);
    const float f31 = F21.at<float>(2,0);
    const float f32 = F21.at<float>(2,1);
    const float f33 = F21.at<float>(2,2);

	//预分配空间
    vbMatchesInliers.resize(N);

	//设置评分初始值（因为后面需要进行这个数值的累计）
    float score = 0;

    // 基于卡方检验计算出的阈值（假设测量有一个像素的偏差？好像不是这样呢）
	// 自由度为1的卡方分布，当平方和有95%的概率不符合正态分布时的阈值
    const float th = 3.841;
	//TODO 这里还增加了一个自由度为2的卡方分布的阈值，但是还不清楚为什么使用这个参与评分，目测应该是和单应矩阵的评分部分统一
    const float thScore = 5.991;

	//计算这个逆，后面计算卡方的时候会用到
    const float invSigmaSquare = 1.0/(sigma*sigma);

	//对于每一对匹配的特征点对
    for(int i=0; i<N; i++)
    {
		//默认为这对特征点是Inliers
        bool bIn = true;

		//从匹配关系中获得索引并且拿到特点数据
        const cv::KeyPoint &kp1 = mvKeys1[mvMatches12[i].first];
        const cv::KeyPoint &kp2 = mvKeys2[mvMatches12[i].second];

		//提取出特征点的坐标
        const float u1 = kp1.pt.x;
        const float v1 = kp1.pt.y;
        const float u2 = kp2.pt.x;
        const float v2 = kp2.pt.y;

        // Reprojection error in second image
        // l2=F21x1=(a2,b2,c2)
        // F21x1可以算出x1在图像中x2对应的线l
		//将参考帧中的特征点以给出的基础矩阵投影到当前帧上，下面的计算完完全全就是矩阵计算的展开
		//注意为了方便计算，这里投影所得到的向量的形式正好是一条2D直线，三个参数对应这直线方程的三个参数
		const float a2 = f11*u1+f12*v1+f13;
        const float b2 = f21*u1+f22*v1+f23;
        const float c2 = f31*u1+f32*v1+f33;

		
        //理想状态下：x2应该在l这条线上:x2点乘l = 0 
		//计算点到直线距离，这里是分子
        const float num2 = a2*u2+b2*v2+c2;

		//计算重投影误差，这里的重投影误差其实是这样子定义的
		//注意这里计算的只有一个平方项
        const float squareDist1 = num2*num2/(a2*a2+b2*b2); // 点到线的几何距离 的平方

		//归一化误差
        const float chiSquare1 = squareDist1*invSigmaSquare;
		
		//判断归一化误差是否大于阈值
		//因为上面计算的只有一个平方项，所以这里的阈值也是选择的服从自由度为1的卡方分布的0.95的阈值
        if(chiSquare1>th)
			//大于就说明这个点是Outlier
            bIn = false;
        else
			//只有小于的时候才认为是Inlier，然后累计对当前使用的基础矩阵的RANSAC评分
			//不过在这里累加的时候使用的阈值还是自由度为2的那个卡方的阈值
			//这样最终计算的结果会使得基础矩阵的比单应矩阵的高（因为前面的那个阈值小）
            score += thScore - chiSquare1;

        // Reprojection error in second image
        // l1 =x2tF21=(a1,b1,c1)
		//然后反过来进行相同操作，求解直线
        const float a1 = f11*u2+f21*v2+f31;
        const float b1 = f12*u2+f22*v2+f32;
        const float c1 = f13*u2+f23*v2+f33;

		//计算分子
        const float num1 = a1*u1+b1*v1+c1;

		//计算重投影误差
        const float squareDist2 = num1*num1/(a1*a1+b1*b1);

		//归一化
        const float chiSquare2 = squareDist2*invSigmaSquare;

		//判断阈值
        if(chiSquare2>th)
            bIn = false;
        else
            score += thScore - chiSquare2;

		//然后就是对点的标记处理
        if(bIn)
            vbMatchesInliers[i]=true;
        else
            vbMatchesInliers[i]=false;
    }//对于每对匹配的特征点

    //返回评分
    return score;
}


//注意下文中的符号“'”表示矩阵的转置
//                          |0 -1  0|
// E = U Sigma V'   let W = |1  0  0|
//                          |0  0  1|
// 得到4个解 E = [R|t]
// R1 = UWV' R2 = UW'V' t1 = U3 t2 = -U3

/**
 * @brief 从F恢复R t
 * 
 * 度量重构
 * 1. 由Fundamental矩阵结合相机内参K，得到Essential矩阵: \f$ E = k'^T F k \f$
 * 2. SVD分解得到R t
 * 3. 进行cheirality check, 从四个解中找出最合适的解
 * 
 * @param vbMatchesInliers	I  	匹配好的特征点对的Inliers标记
 * @param F21				I	从参考帧到当前帧的基础矩阵
 * @param K					I	相机的内参数矩阵
 * @param R21				O	计算好的相机从参考帧到当前帧的旋转
 * @param t21				O	计算好的相机从参考帧到当前帧的平移
 * @param vP3D				O	三角化测量之后的特征点的空间坐标
 * @param vbTriangulated	O	某个特征点是否被三角化了的标记
 * @param minParallax		I	认为三角化测量有效的最小视差角
 * @param minTriangulated	I	认为使用三角化测量进行数据判断的最小测量点数量
 * @return 	是否解析成功
 * 
 * @see Multiple View Geometry in Computer Vision - Result 9.19 p259
 */
bool Initializer::ReconstructF(vector<bool> &vbMatchesInliers, cv::Mat &F21, cv::Mat &K,
                            cv::Mat &R21, cv::Mat &t21, vector<cv::Point3f> &vP3D, vector<bool> &vbTriangulated,
							float minParallax, int minTriangulated)
{
	//计数器，统计被标记为Inlier的特征点对数
    int N=0;
	//开始遍历
    for(size_t i=0, iend = vbMatchesInliers.size() ; i<iend; i++)
		//如果当前被遍历的特征点对被标记，
        if(vbMatchesInliers[i])
			//那么计数器++
            N++;

    // Compute Essential Matrix from Fundamental Matrix
	//根据基础矩阵和相机的内参数矩阵计算本质矩阵
    cv::Mat E21 = K.t()*F21*K;

	//emmm过会儿存放计算结果要用到的
    cv::Mat R1, R2, t;

    // Recover the 4 motion hypotheses
    // 虽然这个函数对t有归一化，但并没有决定单目整个SLAM过程的尺度
    // NOTICE 因为CreateInitialMapMonocular函数对3D点深度会缩放，然后反过来对 t 有改变
	//调用自己建立的解析函数，求解两个R解和两个t解，不过由于两个t解互为相反数，因此这里先只获取一个
    DecomposeE(E21,R1,R2,t);  
	//这里计算另外一个t解
    cv::Mat t1=t;
    cv::Mat t2=-t;

    // Reconstruct with the 4 hyphoteses and check
	//验证
	//这四个向量对应着解的四种组合情况，分别清楚各自情况下三角化测量之后的特征点空间坐标
    vector<cv::Point3f> vP3D1, vP3D2, vP3D3, vP3D4;
	//这四个标记用的向量则保存了哪些点能够被三角化测量的标记
    vector<bool> vbTriangulated1,vbTriangulated2,vbTriangulated3, vbTriangulated4;
	//每种解的情况对应的比较大的特征点对视差角
    float parallax1,parallax2, parallax3, parallax4;

	//检查每种解，会返回一个数值，这个数值是3D点在摄像头前方且投影误差小于阈值的3D点个数，下文我们称之为good点吧
    int nGood1 = CheckRT(R1,t1,							//当前组解
						 mvKeys1,mvKeys2,				//参考帧和当前帧中的特征点
						 mvMatches12, vbMatchesInliers,	//特征点的匹配关系和Inliers标记
						 K, 							//相机的内参数矩阵
						 vP3D1,							//存储三角化以后特征点的空间坐标
						 4.0*mSigma2,					//三角化测量过程中允许的最大重投影误差
						 vbTriangulated1,				//参考帧中被成功进行三角化测量的特征点的标记
						 parallax1);					//认为某对特征点三角化测量有效的最小视差角
    int nGood2 = CheckRT(R2,t1,mvKeys1,mvKeys2,mvMatches12,vbMatchesInliers,K, vP3D2, 4.0*mSigma2, vbTriangulated2, parallax2);
    int nGood3 = CheckRT(R1,t2,mvKeys1,mvKeys2,mvMatches12,vbMatchesInliers,K, vP3D3, 4.0*mSigma2, vbTriangulated3, parallax3);
    int nGood4 = CheckRT(R2,t2,mvKeys1,mvKeys2,mvMatches12,vbMatchesInliers,K, vP3D4, 4.0*mSigma2, vbTriangulated4, parallax4);

	//选取最多的good点数
    int maxGood = max(nGood1,max(nGood2,max(nGood3,nGood4)));

	//清空函数的参数，我们要准备进行输出了
    R21 = cv::Mat();
    t21 = cv::Mat();

    // minTriangulated为可以三角化恢复三维点的个数
	//从这里可以看出minTriangulated变量应该是作为输入的变量
	//然后。。。有这样一个认为应该选取的最少的good点的确定操作
    int nMinGood = max(static_cast<int>(0.9*N),minTriangulated);

	//统计有多少组可行解的，这里暂时称之为“可行解计数变量”吧
    int nsimilar = 0;
	
	//如果在某种情况下观测到的3D点占到了绝大多数，那么“可行解计数”变量++
    if(nGood1>0.7*maxGood)
        nsimilar++;
    if(nGood2>0.7*maxGood)
        nsimilar++;
    if(nGood3>0.7*maxGood)
        nsimilar++;
    if(nGood4>0.7*maxGood)
        nsimilar++;

    // If there is not a clear winner or not enough triangulated points reject initialization
    // 四个结果中如果没有明显的最优结果，则返回失败
    if(maxGood<nMinGood ||		//如果最好的解中没有足够的good点
		nsimilar>1)				//或者是存在两种及以上的解的good点都占了绝大多数，说明没有明显的最优结果
    {
		//认为这次的解算是失败的
        return false;
    }

    // If best reconstruction has enough parallax initialize
    // 比较大的视差角
    //根据后面代码的大概意思，貌似是确定解的时候，必须要有足够的可以被三角化的空间点才行。其实是这样的：
    //下面程序确定解的思想是找大部分空间点在相机前面的解，这个“大部分”是按照0.7*maxGood定义的（当然程序作者
    //考虑问题更加细致，它们还考虑了两种解的大部分空间点都在相机前面的情况），但是如果这里的maxGood本来就不大，那么
    //这个方法其实就没有什么意义了。为了衡量变量maxGood的好坏，这里对每种解都使用了视差角parallax进行描述（因为过小的
    //视差角会带来比较大的观测误差）；然后有函数入口有一个给定的最小值minParallax，如果很幸运某种解的good点占了大多数
    //（其实一般地也就是nGoodx==maxGood了），也要保证parallaxx>minParallax这个条件满足，才能够被认为是真正的解。
    
    //看看最好的good点是在哪种解的条件下发生的
    if(maxGood==nGood1)
    {
		//如果该种解下的parallax大于函数参数中给定的最小值
        if(parallax1>minParallax)
        {
			//那么就它了
			//获取三角测量后的特征点的空间坐标
            vP3D = vP3D1;
			//获取特征点向量的三角化测量标记
            vbTriangulated = vbTriangulated1;

			//另存一份对应解情况下的相机位姿
            R1.copyTo(R21);
            t1.copyTo(t21);
			//返回true表示由给定的基础矩阵求解相机R，t成功
            return true;
        }
    }else if(maxGood==nGood2)					//接下来就是对其他情况的判断了，步骤都是一样的，这里就不加备注了
    {
        if(parallax2>minParallax)
        {
            vP3D = vP3D2;
            vbTriangulated = vbTriangulated2;

            R2.copyTo(R21);
            t1.copyTo(t21);
            return true;
        }
    }else if(maxGood==nGood3)
    {
        if(parallax3>minParallax)
        {
            vP3D = vP3D3;
            vbTriangulated = vbTriangulated3;

            R1.copyTo(R21);
            t2.copyTo(t21);
            return true;
        }
    }else if(maxGood==nGood4)
    {
        if(parallax4>minParallax)
        {
            vP3D = vP3D4;
            vbTriangulated = vbTriangulated4;

            R2.copyTo(R21);
            t2.copyTo(t21);
            return true;
        }
    }

    //如果有最优解但是不满足对应的parallax>minParallax，或者是其他的原因导致的无法求出相机R，t，那么返回false表示求解失败
    return false;
}

// H矩阵分解常见有两种方法：Faugeras SVD-based decomposition 和 Zhang SVD-based decomposition
// 参考文献：Motion and structure from motion in a piecewise plannar environment
// 这篇参考文献和下面的代码使用了Faugeras SVD-based decomposition算法

/**
 * @brief 从H恢复R t
 * @param vbMatchesInliers	I	匹配点对的内点标记
 * @param H21				I  	从参考帧到当前帧的单应矩阵
 * @param K					I	相机的内参数矩阵
 * @param R21				O	计算出来的相机旋转
 * @param t21				O	计算出来的相机平移
 * @param vP3D				O	世界坐标系下，三角化测量特征点对之后得到的特征点的空间坐标
 * @param vbTriangulated	O	特征点对被三角化测量的标记
 * @param minParallax		I	在进行三角化测量时，观测正常所允许的最小视差角
 * @param minTriangulated	I	最少被三角化的点对数（其实也是点个数）
 * @return	数据解算是否成功的
 * @see
 * - Faugeras et al, Motion and structure from motion in a piecewise planar environment. International Journal of Pattern Recognition and Artificial Intelligence, 1988.
 * - Deeper understanding of the homography decomposition for vision-based control
 */
bool Initializer::ReconstructH(vector<bool> &vbMatchesInliers, cv::Mat &H21, cv::Mat &K,
                      cv::Mat &R21, cv::Mat &t21, vector<cv::Point3f> &vP3D, vector<bool> &vbTriangulated, 
					  float minParallax, int minTriangulated)
{
	//匹配的特征点对中属于Inlier的个数
    int N=0;
	//遍历
    for(size_t i=0, iend = vbMatchesInliers.size() ; i<iend; i++)
		//如果被遍历到的这个点属于Inlier
        if(vbMatchesInliers[i])
			//计数变量++
            N++;

    // We recover 8 motion hypotheses using the method of Faugeras et al.
    // Motion and structure from motion in a piecewise planar environment.
    // International Journal of Pattern Recognition and Artificial Intelligence, 1988

    // 因为特征点是图像坐标系，所以将H矩阵由相机坐标系换算到图像坐标系
    cv::Mat invK = K.inv();
	//这个部分不要看PPT，对不太上，可以结合着视觉SLAM十四讲P146页中单应矩阵的推导来看 
	//TODO  不过感觉最后还是缺少了一个因子d
    cv::Mat A = invK*H21*K;

	//存储进行奇异值分解的结果
    cv::Mat U,w,Vt,V;
	//进行奇异值分解
    cv::SVD::compute(A,						//等待被进行奇异值分解的矩阵
					 w,						//奇异值矩阵
					 U,						//奇异值分解左矩阵
					 Vt,					//奇异值分解右矩阵，注意函数返回的是转置
					 cv::SVD::FULL_UV);		//全部分解
	//得到奇异值分解的右矩阵本身
    V=Vt.t();

	//计算矩阵U的行列式的值和矩阵V^t的行列式的值的乘积
	//|V|==|Vt|
    float s = cv::determinant(U)*cv::determinant(Vt);
	
	//取得矩阵的各个奇异值
    float d1 = w.at<float>(0);
    float d2 = w.at<float>(1);
    float d3 = w.at<float>(2);

    // SVD分解的正常情况是特征值降序排列
	// NOTICE 妙啊！！！用这种方式来比较浮点数的大小
    if(d1/d2<1.00001 || d2/d3<1.00001)
    {
		//如果不满足奇异值的降序排列则说明计算错误，解算失败，返回false
        return false;
    }

    //由于d1 d2 d3的关系再怎么特殊，计算过程也还是都一样的，因此这里没有进行判断而是直接进行处理了
    //存储每一种子情况下解出来的旋转矩阵、平移向量和空间向量
    vector<cv::Mat> vR, vt, vn;
	//预分配空间
    vR.reserve(8);
    vt.reserve(8);
    vn.reserve(8);

    //n'=[x1 0 x3] 4 posibilities e1=e3=1, e1=1 e3=-1, e1=-1 e3=1, e1=e3=-1
    // 法向量n'= [x1 0 x3] 对应ppt的公式17
	//上面注释中的e1 e3其实就是PPT中公式17的表示+-1的符号变量
	//x1的未加符号版
    float aux1 = sqrt((d1*d1-d2*d2)/(d1*d1-d3*d3));
    float aux3 = sqrt((d2*d2-d3*d3)/(d1*d1-d3*d3));
	//下面两个数组则是存储了e1 e3相互搭配可能会出现的四种解的情况
    float x1[] = {aux1,aux1,-aux1,-aux1};
    float x3[] = {aux3,-aux3,aux3,-aux3};

    //case d'=+d2
    // 计算ppt中公式19
	//当确定了d'=d2时，PPT中公式19中sin_theta项的无符号版本
    float aux_stheta = sqrt((d1*d1-d2*d2)*(d2*d2-d3*d3))/((d1+d3)*d2);
	//cos_theta项
    float ctheta = (d2*d2+d1*d3)/((d1+d3)*d2);
	//根据e1 e3的各种不同的排列组合，可能的sin_thet项的两种解，这里因为后面计算方便就写成了这种形式
    float stheta[] = {aux_stheta, -aux_stheta, -aux_stheta, aux_stheta};

    // 计算旋转矩阵 R‘，计算ppt中公式18
	//根据不同的e1 e3组合所得出来的四种R t的解
    //      | ctheta      0   -aux_stheta|       | aux1|
    // Rp = |    0        1       0      |  tp = |  0  |
    //      | aux_stheta  0    ctheta    |       |-aux3|

    //      | ctheta      0    aux_stheta|       | aux1|
    // Rp = |    0        1       0      |  tp = |  0  |
    //      |-aux_stheta  0    ctheta    |       | aux3|

    //      | ctheta      0    aux_stheta|       |-aux1|
    // Rp = |    0        1       0      |  tp = |  0  |
    //      |-aux_stheta  0    ctheta    |       |-aux3|

    //      | ctheta      0   -aux_stheta|       |-aux1|
    // Rp = |    0        1       0      |  tp = |  0  |
    //      | aux_stheta  0    ctheta    |       | aux3|
	// 开始遍历这四种情况中的每一种
    for(int i=0; i<4; i++)
    {
		//生成Rp，就是PPT中公式的 R‘
        cv::Mat Rp=cv::Mat::eye(3,3,CV_32F);
        Rp.at<float>(0,0)=ctheta;
        Rp.at<float>(0,2)=-stheta[i];		//在这里你就明白为什么前面非得用数组表示了
        Rp.at<float>(2,0)=stheta[i];		
        Rp.at<float>(2,2)=ctheta;

		//这里的变量定义和PPT中的不同，看原始论文：
		//Motion and structure from motion in a piecewise planner environment
        cv::Mat R = s*U*Rp*Vt;
		//将这个“真实”的R添加到相关的向量中
        vR.push_back(R);

		//生成tp
        cv::Mat tp(3,1,CV_32F);
        tp.at<float>(0)=x1[i];
        tp.at<float>(1)=0;
        tp.at<float>(2)=-x3[i];
        tp*=d1-d3;

        // 这里虽然对t有归一化，并没有决定单目整个SLAM过程的尺度
        // 因为CreateInitialMapMonocular函数对3D点深度会缩放，然后反过来对 t 有改变
		//恢复原始的t
        cv::Mat t = U*tp;
		//首先进行向量自归一化，然后添加到vector中
        vt.push_back(t/cv::norm(t));

		//构造法向量np
        cv::Mat np(3,1,CV_32F);
        np.at<float>(0)=x1[i];
        np.at<float>(1)=0;
        np.at<float>(2)=x3[i];

		//恢复原始的法向量
        cv::Mat n = V*np;
		//看PPT 16页的图，保持平面法向量向上
        if(n.at<float>(2)<0)
            n=-n;
		//添加到vector
        vn.push_back(n);
    }//对于由e1 e3导致的每种可能的解
    
    // 

    //case d'=-d2
    // 计算ppt中 公式22，sin_theta的无符号版
    float aux_sphi = sqrt((d1*d1-d2*d2)*(d2*d2-d3*d3))/((d1-d3)*d2);
	//cos_theta项
    float cphi = (d1*d3-d2*d2)/((d1-d3)*d2);
	//考虑到e1,e2的取值，这里的sin_theta有两种可能的解，但是为了下面的计算方便需要，这里写成了这种形式
    float sphi[] = {aux_sphi, -aux_sphi, -aux_sphi, aux_sphi};

    //对于每种由e1 e3取值的组合而形成的四种解的情况
    for(int i=0; i<4; i++)
    {
		// 计算旋转矩阵 R‘，计算ppt中公式21
        cv::Mat Rp=cv::Mat::eye(3,3,CV_32F);
        Rp.at<float>(0,0)=cphi;
        Rp.at<float>(0,2)=sphi[i];
        Rp.at<float>(1,1)=-1;
        Rp.at<float>(2,0)=sphi[i];
        Rp.at<float>(2,2)=-cphi;

		//恢复出原来的R
        cv::Mat R = s*U*Rp*Vt;
		//然后添加到vector中
        vR.push_back(R);

		//构造tp
        cv::Mat tp(3,1,CV_32F);
        tp.at<float>(0)=x1[i];
        tp.at<float>(1)=0;
        tp.at<float>(2)=x3[i];
        tp*=d1+d3;

		//恢复出原来的t
        cv::Mat t = U*tp;
		//归一化之后加入到vector中,要提供给上面的平移矩阵都是要进行过归一化的
        vt.push_back(t/cv::norm(t));

		//构造法向量np
        cv::Mat np(3,1,CV_32F);
        np.at<float>(0)=x1[i];
        np.at<float>(1)=0;
        np.at<float>(2)=x3[i];

		//恢复出原来的法向量
        cv::Mat n = V*np;
		//保证法向量指向上方
        if(n.at<float>(2)<0)
            n=-n;
		//添加到vector中
        vn.push_back(n);
    }

	//最好的good点
    int bestGood = 0;
	//其次最好的good点
    int secondBestGood = 0;    
	//最好的解的索引，初始值为-1
    int bestSolutionIdx = -1;
	//最大的视差角
    float bestParallax = -1;
	//存储最好解对应的，对特征点对进行三角化测量的结果
    vector<cv::Point3f> bestP3D;
	//最佳解所对应的，那些可以被三角化测量的点的标记
    vector<bool> bestTriangulated;

    // Instead of applying the visibility constraints proposed in the WFaugeras' paper (which could fail for points seen with low parallax)
    // We reconstruct all hypotheses and check in terms of triangulated points and parallax
	
    // d'=d2和d'=-d2分别对应8组(R t)
	//分分别验证每一种解的情况
    for(size_t i=0; i<8; i++)
    {
		//第i组解对应的比较大的视差角
        float parallaxi;
		//三角化测量之后的特征点的空间坐标
        vector<cv::Point3f> vP3Di;
		//特征点对是否被三角化的标记
        vector<bool> vbTriangulatedi;
		//调用检查的函数，计算good点的数目
        int nGood = CheckRT(vR[i],vt[i],					//当前组解的旋转矩阵和平移向量
							mvKeys1,mvKeys2,				//特征点
							mvMatches12,vbMatchesInliers,	//特征匹配关系以及Inlier标记
							K,								//相机的内参数矩阵
							vP3Di, 							//存储三角化测量之后的特征点空间坐标的
							4.0*mSigma2,					//三角化过程中允许的最大重投影误差
							vbTriangulatedi,				//特征点是否被成功进行三角测量的标记
							parallaxi);						// 这组解在三角化测量的时候的比较大的视差角
        // 保留最优的和次优的
		//保存次优解的目的是看看最优解是否突出
		//如果当前组解的good点数是历史最优
        if(nGood>bestGood)
        {
			//那么之前的历史最优就变成了历史次优
            secondBestGood = bestGood;
			//更新历史最优点
            bestGood = nGood;
			//最优解的组索引为i（就是当前次遍历）
            bestSolutionIdx = i;
			//更新
            bestParallax = parallaxi;
			//更新
            bestP3D = vP3Di;
			//更新
            bestTriangulated = vbTriangulatedi;
        }
        //如果当前组的good计数小于历史最优但却大于历史次优
        else if(nGood>secondBestGood)
        {
			//说明当前组解是历史次优点，更新之
            secondBestGood = nGood;
        }
    }//分别验证每一组解的情况


    //要满足以下条件：
    if(secondBestGood<0.75*bestGood && 		//最优解的good数要足够突出
	   bestParallax>=minParallax && 		//最好的视角差大于规定的阈值
	   bestGood>minTriangulated && 			//最优解的good点数要大于规定的最小的被三角化的点数量
	   bestGood>0.9*N)						//最优解的good数要足够多，达到9成以上
    {
		//只有上面的四个条件同时被满足时，我们才能够认为这组解是真正的最好的解、是满足要求的解
		//从最佳的解的索引访问到R，t
        vR[bestSolutionIdx].copyTo(R21);
        vt[bestSolutionIdx].copyTo(t21);
		//获得最佳解时，对特征点三角化测量得到的空间坐标
        vP3D = bestP3D;
		//获取特征点的被成功进行三角化的标记
        vbTriangulated = bestTriangulated;

		//返回真，找到了最好的解
        return true;
    }
	//没有找到，返回false
    return false;
}


//其实下面的这些推导，看PPT上讲得也许会更好
// Trianularization: 已知匹配特征点对{x x'} 和 各自相机矩阵{P P'}, 估计三维点 X
// x' = P'X  x = PX
// 它们都属于 x = aPX模型
//                         |X|
// |x|     |p1 p2  p3  p4 ||Y|     |x|    |--p0--||.|
// |y| = a |p5 p6  p7  p8 ||Z| ===>|y| = a|--p1--||X|
// |z|     |p9 p10 p11 p12||1|     |z|    |--p2--||.|
// 采用DLT的方法：x叉乘PX = 0
// |yp2 -  p1|     |0|
// |p0 -  xp2| X = |0|
// |xp1 - yp0|     |0|
// 两个点:
// |yp2   -  p1  |     |0|
// |p0    -  xp2 | X = |0| ===> AX = 0
// |y'p2' -  p1' |     |0|
// |p0'   - x'p2'|     |0|
// 变成程序中的形式：
// |xp2  - p0 |     |0|
// |yp2  - p1 | X = |0| ===> AX = 0
// |x'p2'- p0'|     |0|
// |y'p2'- p1'|     |0|
// 然后就组成了一个四元一次正定方程组，求解呗
/**
 * @brief 给定投影矩阵P1,P2和图像上的点kp1,kp2，从而恢复3D坐标
 *
 * @param kp1 特征点, in reference frame
 * @param kp2 特征点, in current frame
 * @param P1  投影矩阵P1
 * @param P2  投影矩阵P2
 * @param x3D 三维点
 * @see       Multiple View Geometry in Computer Vision - 12.2 Linear triangulation methods p312
 */
void Initializer::Triangulate(const cv::KeyPoint &kp1, const cv::KeyPoint &kp2, const cv::Mat &P1, const cv::Mat &P2, cv::Mat &x3D)
{
	// NOTICE
    // 在DecomposeE函数和ReconstructH函数中对t有归一化
    // 这里三角化过程中恢复的3D点深度取决于 t 的尺度，
    // 但是这里恢复的3D点并没有决定单目整个SLAM过程的尺度
    // 因为CreateInitialMapMonocular函数对3D点深度会缩放，然后反过来对 t 有改变

	//这个就是上面注释中的矩阵A
    cv::Mat A(4,4,CV_32F);

	//构造参数矩阵A
    A.row(0) = kp1.pt.x*P1.row(2)-P1.row(0);
    A.row(1) = kp1.pt.y*P1.row(2)-P1.row(1);
    A.row(2) = kp2.pt.x*P2.row(2)-P2.row(0);
    A.row(3) = kp2.pt.y*P2.row(2)-P2.row(1);

	//奇异值分解的结果
    cv::Mat u,w,vt;
	//对系数矩阵A进行奇异值分解
    cv::SVD::compute(A,w,u,vt,cv::SVD::MODIFY_A| cv::SVD::FULL_UV);
	//根据前面的结论，奇异值分解右矩阵的最后一行其实就是解，原理类似于前面的求最小二乘解，四个未知数四个方程正好正定
	//别忘了我们更习惯用列向量来表示一个点的空间坐标
    x3D = vt.row(3).t();
	//为了符合其次坐标的形式，使最后一维为1
    x3D = x3D.rowRange(0,3)/x3D.at<float>(3);
}

/**
 * ＠brief 归一化特征点到同一尺度（作为normalize DLT的输入）
 *
 * [x' y' 1]' = T * [x y 1]' \n
 * 归一化后x', y'的均值为0，sum(abs(x_i'-0))=1，sum(abs((y_i'-0))=1
 * 
 * @param vKeys             特征点在图像上的坐标
 * @param vNormalizedPoints 特征点归一化后的坐标
 * @param T                 将特征点归一化的矩阵
 */
void Initializer::Normalize(const vector<cv::KeyPoint> &vKeys, vector<cv::Point2f> &vNormalizedPoints, cv::Mat &T)
{
	//X、Y坐标的均值
    float meanX = 0;
    float meanY = 0;
	//获取特征点的数量
    const int N = vKeys.size();

	//根据这个数目来设置用来存储归一后特征点的向量大小
    vNormalizedPoints.resize(N);

	//开始遍历所有的特征点
    for(int i=0; i<N; i++)
    {
		//分别累加特征点的X、Y坐标
        meanX += vKeys[i].pt.x;
        meanY += vKeys[i].pt.y;
    }

    //计算X、Y坐标的均值
    meanX = meanX/N;
    meanY = meanY/N;

	//累计累计这些特征点偏离横纵坐标均值的程度的变量
    float meanDevX = 0;
    float meanDevY = 0;

    // 将所有vKeys点减去中心坐标，使x坐标和y坐标均值分别为0
    for(int i=0; i<N; i++)
    {
        vNormalizedPoints[i].x = vKeys[i].pt.x - meanX;
        vNormalizedPoints[i].y = vKeys[i].pt.y - meanY;

		//累计这些特征点偏离横纵坐标均值的程度
        meanDevX += fabs(vNormalizedPoints[i].x);
        meanDevY += fabs(vNormalizedPoints[i].y);
    }

    //求出平均到每个点上，其坐标偏离横纵坐标均值的程度
    meanDevX = meanDevX/N;
    meanDevY = meanDevY/N;

	//取其倒数作为一个尺度缩放因子
    float sX = 1.0/meanDevX;
    float sY = 1.0/meanDevY;

    // 将x坐标和y坐标分别进行尺度缩放，使得x坐标和y坐标的一阶绝对矩分别为1
	//这里所谓的一阶绝对矩其实就是随机变量到取值的中心的绝对值的平均值
	//归一化就体现在这里
    for(int i=0; i<N; i++)
    {
		//对，就是简单地对特征点的坐标进行进一步的缩放
        vNormalizedPoints[i].x = vNormalizedPoints[i].x * sX;
        vNormalizedPoints[i].y = vNormalizedPoints[i].y * sY;
    }

    // |sX  0  -meanx*sX|
    // |0   sY -meany*sY|
    // |0   0      1    |
    T = cv::Mat::eye(3,3,CV_32F);
    T.at<float>(0,0) = sX;
    T.at<float>(1,1) = sY;
    T.at<float>(0,2) = -meanX*sX;
    T.at<float>(1,2) = -meanY*sY;
}

/**
 * @brief 进行cheirality check，从而进一步找出F分解后最合适的解
 * @param R					I	待检查的相机旋转矩阵R
 * @param t					I	待检查的相机旋转矩阵t
 * @param vKeys1			I	参考帧特征点
 * @param vKeys2			I	当前帧特征点
 * @param vMatches12		I	两帧特征点的匹配关系
 * @param vbMatchesInliers	I	特征点对的Inliers标记
 * @param K					I	相机的内参数矩阵
 * @param vP3D				O	三角化测量之后的特征点的空间坐标
 * @param th2				I	重投影误差的阈值
 * @param vbGood			O	特征点（对）中是good点的标记
 * @param parallax			O 	计算出来的比较大的视差角（注意不是最大，这个要看后面中程序的注释）
 * @return	返回本组解中good点的数目
 */
int Initializer::CheckRT(const cv::Mat &R, const cv::Mat &t, 
					     const vector<cv::KeyPoint> &vKeys1, const vector<cv::KeyPoint> &vKeys2,
                         const vector<Match> &vMatches12, vector<bool> &vbMatchesInliers,
                         const cv::Mat &K, vector<cv::Point3f> &vP3D, float th2, vector<bool> &vbGood, float &parallax)
{
    // Calibration parameters
	//从相机内参数矩阵获取相机的校正参数
    const float fx = K.at<float>(0,0);
    const float fy = K.at<float>(1,1);
    const float cx = K.at<float>(0,2);
    const float cy = K.at<float>(1,2);

	//特征点是否是good点的标记，这里的特征点指的是参考帧中的特征点
    vbGood = vector<bool>(vKeys1.size(),false);
	//重设存储空间坐标的点的大小
    vP3D.resize(vKeys1.size());

	//存储计算出来的每对特征点的视差
    vector<float> vCosParallax;
	//然后预分配空间
    vCosParallax.reserve(vKeys1.size());

    // Camera 1 Projection Matrix K[I|0]
    // 步骤1：得到一个相机的投影矩阵
    // 以第一个相机的光心作为世界坐标系
	//定义相机的投影矩阵
    cv::Mat P1(3,4,				//矩阵的大小是3x4
			   CV_32F,			//数据类型是浮点数
			   cv::Scalar(0));	//初始的数值是0
	//将整个K矩阵拷贝到P1矩阵的(0,0)~(2,2)，K*[I|0]
    K.copyTo(P1.rowRange(0,3).colRange(0,3));
    // 第一个相机的光心在世界坐标系下的坐标(对于目前的应用的问题来说，这个坐标其实就是原点)
    cv::Mat O1 = cv::Mat::zeros(3,1,CV_32F);

    // Camera 2 Projection Matrix K[R|t]
    // 步骤2：得到第二个相机的投影矩阵
	//定义
    cv::Mat P2(3,4,CV_32F);
	//生成
    R.copyTo(P2.rowRange(0,3).colRange(0,3));
    t.copyTo(P2.rowRange(0,3).col(3));
	//最终结果是K*[R|t]
    P2 = K*P2;
    // 第二个相机的光心在世界坐标系下的坐标
	//TODO 不太理解？？
    cv::Mat O2 = -R.t()*t;

	//在遍历开始前，先将good点计数设置为0
    int nGood=0;

	//开始针对特征点对展开遍历
    for(size_t i=0, iend=vMatches12.size();i<iend;i++)
    {
		//如果当前的特征点对是噪声点，即不是Inliers
        if(!vbMatchesInliers[i])
			//那么就跳过，进行下一个特征点对的遍历
            continue;

        // kp1和kp2是匹配特K[R|t]征点
		//如果是Inliers就根据存储的索引关系拿到两个特征点
        const cv::KeyPoint &kp1 = vKeys1[vMatches12[i].first];
        const cv::KeyPoint &kp2 = vKeys2[vMatches12[i].second];
		//存储三维点的的坐标
        cv::Mat p3dC1;

        // 步骤3：利用三角法恢复三维点p3dC1
        Triangulate(kp1,kp2,	//特征点
					P1,P2,		//投影矩阵
					p3dC1);		//输出，三角化测量之后特征点的空间坐标		

		// NOTICE 下面的这个isfinite()貌似确实没有被定义过啊，是C++中提供的函数吗
		//只要这个三角化测量结果的空间坐标中有一项是无穷大的，
        if(!isfinite(p3dC1.at<float>(0)) || !isfinite(p3dC1.at<float>(1)) || !isfinite(p3dC1.at<float>(2)))
        {
			//那么就说明三角化失败，原因不明
			//其实这里就算是不这样写也没问题，因为默认的匹配点对就不是good点
            vbGood[vMatches12[i].first]=false;
			//继续对下一对匹配点的处理
            continue;
        }

        // Check parallax
        // 步骤4：计算视差角余弦值
        //得到向量PO1
        cv::Mat normal1 = p3dC1 - O1;
		//求取模长，其实就是距离
        float dist1 = cv::norm(normal1);

		//同理构造向量PO2
        cv::Mat normal2 = p3dC1 - O2;
		//求模长
        float dist2 = cv::norm(normal2);

		//根据公式：a.*b=|a||b|cos_theta 可以推导出来下面的式子
        float cosParallax = normal1.dot(normal2)/(dist1*dist2);

        // 步骤5：判断3D点是否在两个摄像头前方
        // Check depth in front of first camera (only if enough parallax, as "infinite" points can easily go to negative depth)
        // 步骤5.1：3D点深度为负，在第一个摄像头后方，淘汰
        if(p3dC1.at<float>(2)<=0 && 		//3D点深度为负
			cosParallax<0.99998)			//并且还要有一定的视差角 
											//原因在下面会提到：一般视差角比较小时重投影误差比较大
			//然后就不用这个点了，直接淘汰进行下一个点
            continue;

        // Check depth in front of second camera (only if enough parallax, as "infinite" points can easily go to negative depth)
        // 步骤5.2：3D点深度为负，在第二个摄像头后方，淘汰
        cv::Mat p3dC2 = R*p3dC1+t;			//注意这里是空间点的旋转和平移变换
		//判断过程和上面的相同
        if(p3dC2.at<float>(2)<=0 && cosParallax<0.99998)
            continue;

        // 步骤6：计算重投影误差
        // Check reprojection error in first image
        // 计算3D点在第一个图像上的投影误差
		//投影到参考帧图像上的点的坐标x,y
        float im1x, im1y;
		//这个使能空间点的z坐标的倒数
        float invZ1 = 1.0/p3dC1.at<float>(2);
		//投影到参考帧图像上。因为参考帧下的相机坐标系和世界坐标系重合，因此这里就直接进行投影就可以了
        im1x = fx*p3dC1.at<float>(0)*invZ1+cx;
        im1y = fy*p3dC1.at<float>(1)*invZ1+cy;

		//参考帧上的重投影误差，这个的确就是按照定义来的
        float squareError1 = (im1x-kp1.pt.x)*(im1x-kp1.pt.x)+(im1y-kp1.pt.y)*(im1y-kp1.pt.y);

        // 步骤6.1：重投影误差太大，跳过淘汰
        // 一般视差角比较小时重投影误差比较大
        if(squareError1>th2)
            continue;

        // Check reprojection error in second image
        // 计算3D点在第二个图像上的投影误差
        float im2x, im2y;
		//逆
        float invZ2 = 1.0/p3dC2.at<float>(2);
		//同样的计算过程
        im2x = fx*p3dC2.at<float>(0)*invZ2+cx;
        im2y = fy*p3dC2.at<float>(1)*invZ2+cy;

		//计算同样的重投影误差
        float squareError2 = (im2x-kp2.pt.x)*(im2x-kp2.pt.x)+(im2y-kp2.pt.y)*(im2y-kp2.pt.y);

        // 步骤6.2：重投影误差太大，跳过淘汰
        // 一般视差角比较小时重投影误差比较大
        if(squareError2>th2)
            continue;

        // 步骤7：统计经过检验的3D点个数，记录3D点视差角
		//如果运行到这里就说明当前遍历的这个特征点对的性质不错，经过了重重检验，说明是一个合格的good点
        vCosParallax.push_back(cosParallax);
		//存储这个三角化测量后的3D点在世界坐标系下的坐标
        vP3D[vMatches12[i].first] = cv::Point3f(p3dC1.at<float>(0),p3dC1.at<float>(1),p3dC1.at<float>(2));
		//good点计数++
        nGood++;

		//判断视差角，只有视差角稍稍大一丢丢的才会给打good点标记
		//不过我觉得这个写的位置不太对。你的good点计数都++了然后才判断，不是会让good点标志和good点计数不一样吗
        if(cosParallax<0.99998)
            vbGood[vMatches12[i].first]=true;
    }//针对特征点对展开遍历

    // 步骤8：得到3D点中较大的视差角
    if(nGood>0)
    {
        // 从小到大排序
        sort(vCosParallax.begin(),vCosParallax.end());

        // NOTICE trick! 排序后并没有取最大的视差角
        // 取一个较大的视差角
		// 作者的想法是，如果经过检验过后的视差角个数小于50个，那么就取最后那个最大的视差角
		//如果大于50个，就取视差角中的排名第50小的，足够大就可以没有必要非得要最大的—— TODO 可这是为什么呢？
        size_t idx = min(50,int(vCosParallax.size()-1));
		//将这个选中的角由cos值转化为弧度制再转换为角度制
        parallax = acos(vCosParallax[idx])*180/CV_PI;
    }
    else
		//如果没有good点那么这个就直接设置为0了
        parallax=0;

	//返回good点计数
    return nGood;
}

/**
 * @brief 分解Essential矩阵
 * 
 * F矩阵通过结合内参可以得到Essential矩阵，分解E矩阵将得到4组解 \n
 * 这4组解分别为[R1,t],[R1,-t],[R2,t],[R2,-t]
 * @param E  Essential Matrix
 * @param R1 Rotation Matrix 1
 * @param R2 Rotation Matrix 2
 * @param t  Translation，另外一个结果取它的相反数就行
 * @see Multiple View Geometry in Computer Vision - Result 9.19 p259
 */
void Initializer::DecomposeE(const cv::Mat &E, cv::Mat &R1, cv::Mat &R2, cv::Mat &t)
{
	//准备存储对本质矩阵进行奇异值分解的结果
    cv::Mat u,w,vt;
	//对本质矩阵进行奇异值分解
    cv::SVD::compute(E,w,u,vt);

    // 对 t 有归一化，但是这个地方并没有决定单目整个SLAM过程的尺度
    // 因为CreateInitialMapMonocular函数对3D点深度会缩放，然后反过来对 t 有改变
	//其实这里的t就是左奇异矩阵的最后一列
    u.col(2).copyTo(t);
	//然后对t进行归一化
    t=t/cv::norm(t);

	//构造一个绕Z轴旋转pi/2的旋转矩阵
    cv::Mat W(3,3,CV_32F,cv::Scalar(0));
    W.at<float>(0,1)=-1;
    W.at<float>(1,0)=1;
    W.at<float>(2,2)=1;

	//计算
    R1 = u*W*vt;
	//检查旋转矩阵行列式的数值
    if(cv::determinant(R1)<0) // 旋转矩阵有行列式为1的约束
        R1=-R1;

	//同理，计算
    R2 = u*W.t()*vt;
	//检查旋转矩阵行列式的数值
    if(cv::determinant(R2)<0)
        R2=-R2;
}

} //namespace ORB_SLAM