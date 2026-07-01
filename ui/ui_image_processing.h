#pragma once

#include "ui.h"

/**
 * @brief UI 显示与分析使用的图像转换接口
 */
namespace ui_detail
{
    /**
     * @brief 将源帧转换为指定视图对应的三通道 BGR 图像
     *
     * @param frame 任意受支持类型和通道数的源图像
     * @param mode 颜色空间或单通道显示方式
     *
     * @return 可直接上传为纹理的 CV_8UC3 图像
     */
    cv::Mat makeDisplayImage(const cv::Mat &frame, ViewMode mode);

    /**
     * @brief 根据源图像亮度在显示图像上覆盖欠曝和过曝标记
     *
     * @param display 接收标记颜色的 CV_8UC3 显示图像
     * @param source 生成亮度掩码的源图像
     * @param shadowThreshold 欠曝像素的灰度上限
     * @param highlightThreshold 过曝像素的灰度下限
     */
    void applyExposureWarnings(cv::Mat &display, const cv::Mat &source, int shadowThreshold,
                               int highlightThreshold);

    /**
     * @brief 计算当前模式的直方图并绘制为 BGR 图像
     *
     * @param frame 参与统计的整帧或 ROI 图像
     * @param mode 决定统计通道与绘图颜色的显示模式
     * @param state 可选的应用状态，用于同步悬停查询缓存
     *
     * @return 固定布局的 CV_8UC3 直方图图像
     */
    cv::Mat makeHistogram(const cv::Mat &frame, ViewMode mode, AppState *state = nullptr);
}
