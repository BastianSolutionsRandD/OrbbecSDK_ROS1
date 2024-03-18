﻿/**
 * @file  Filter.hpp
 * @brief This file contains the Filter class, which is the processing unit of the SDK that can perform point cloud generation, format conversion, and other
 * functions.
 */
#pragma once

#include "Types.hpp"
#include <functional>
#include <memory>
#include <map>
#include <string>

namespace ob {
class Frame;
class OBFilterList;

/**
 * @brief A callback function that takes a shared pointer to a Frame object as its argument.
 */
typedef std::function<void(std::shared_ptr<Frame>)> FilterCallback;

/**
 * @brief The Filter class is the base class for all filters in the SDK.
 */
class OB_EXTENSION_API Filter : public std::enable_shared_from_this<Filter> {
public:
    Filter();

    Filter(std::shared_ptr<FilterImpl> impl);

    virtual ~Filter() = default;

    /**
     * @brief ReSet the filter, freeing the internal cache, stopping the processing thread, and clearing the pending buffer frame when asynchronous processing
     * is used.
     */
    virtual void reset();

    /**
     * @brief enable the filter
     */
    void enable(bool enable);

    /**
     * @brief Return Enable State
     */
    bool isEnabled();

    /**
     * @brief Processes a frame synchronously.
     *
     * @param frame The frame to be processed.
     * @return std::shared_ptr< Frame > The processed frame.
     */
    virtual std::shared_ptr<Frame> process(std::shared_ptr<Frame> frame);

    /**
     * @brief Pushes the pending frame into the cache for asynchronous processing.
     *
     * @param frame The pending frame. The processing result is returned by the callback function.
     */
    virtual void pushFrame(std::shared_ptr<Frame> frame);

    /**
     * @brief Set the callback function for asynchronous processing.
     *
     * @param callback The processing result callback.
     */
    virtual void setCallBack(FilterCallback callback);

    /**
     * @brief Get the type of filter.
     *
     * @return string The type of filte.
     */
    virtual std::string type();

    /**
     * @brief Check if the runtime type of the filter object is compatible with a given type.
     *
     * @tparam T The given type.
     * @return bool The result.
     */
    template <typename T> bool is();

    /**
     * @brief Convert the filter object to a target type.
     *
     * @tparam T The target type.
     * @return std::shared_ptr<T> The result. If it cannot be converted, an exception will be thrown.
     */
    template <typename T> std::shared_ptr<T> as() {
        if(!is<T>()) {
            throw std::runtime_error("unsupported operation, object's type is not require type");
        }

        return std::static_pointer_cast<T>(shared_from_this());
    }

protected:
    std::shared_ptr<FilterImpl> impl_;
    std::string type_;

friend class OBFilterList;
};

/**
 * @brief The PointCloudFilter class is a subclass of Filter that generates point clouds.
 */
class OB_EXTENSION_API PointCloudFilter : public Filter {
public:
    PointCloudFilter();

    /**
     * @brief Set the point cloud type parameters.
     *
     * @param type The point cloud type: depth point cloud or RGBD point cloud.
     */
    void setCreatePointFormat(OBFormat type);

    /**
     * @brief Set the camera parameters.
     *
     * @param param The camera internal and external parameters.
     */
    void setCameraParam(OBCameraParam param);

    /**
     * @brief Set the frame alignment state that will be input to generate point cloud.
     *
     * @param state The alignment status. True: enable alignment; False: disable alignment.
     */
    void setFrameAlignState(bool state);

    /**
     * @brief Set the point cloud coordinate data zoom factor.
     *
     * @attention Calling this function to set the scale will change the point coordinate scaling factor of the output point cloud frame: posScale = posScale /
     * scale. The point coordinate scaling factor for the output point cloud frame can be obtained via @ref PointsFrame::getPositionValueScale function.
     *
     * @param scale The zoom factor.
     */
    void setPositionDataScaled(float scale);

    /**
     * @brief Set point cloud color data normalization.
     *
     * @param state Whether normalization is required.
     */
    void setColorDataNormalization(bool state);

    /**
     * @brief Set the point cloud coordinate system.
     *
     * @param type The coordinate system type.
     */
    void setCoordinateSystem(OBCoordinateSystemType type);
};

/**
 * @brief The FormatConvertFilter class is a subclass of Filter that performs format conversion.
 */
class OB_EXTENSION_API FormatConvertFilter : public Filter {
public:
    FormatConvertFilter();

    /**
     * @brief Set the format conversion type.
     *
     * @param type The format conversion type.
     */
    void setFormatConvertType(OBConvertFormat type);
};

/**
 * @brief The CompressionFilter class is a subclass of Filter that performs compression.
 */
class OB_EXTENSION_API CompressionFilter : public Filter {
public:
    CompressionFilter();

    /**
     * @brief Set the compression parameters.
     *
     * @param mode The compression mode: OB_COMPRESSION_LOSSLESS or OB_COMPRESSION_LOSSY.
     * @param params The compression parameters. When mode is OB_COMPRESSION_LOSSLESS, params is NULL.
     */
    void setCompressionParams(OBCompressionMode mode, void *params);
};

/**
 * @brief The DecompressionFilter class is a subclass of Filter that performs decompression.
 */
class OB_EXTENSION_API DecompressionFilter : public Filter {
public:
    DecompressionFilter();
};

/**
 * @brief Hole filling filter
 */
class OB_EXTENSION_API HoleFillingFilter : public Filter {
public:
    HoleFillingFilter();

    void setFilterMode(OBHoleFillingMode mode);

    OBHoleFillingMode getFilterMode();
};

/**
 * @brief Temporal filter
 */
class OB_EXTENSION_API TemporalFilter : public Filter {
public:
    TemporalFilter();

    OBFloatPropertyRange getDiffScaleRange();

    void setDiffScale(float value);

    OBFloatPropertyRange getWeightRange();

    void setWeight(float value);
    
};

/**
 * @brief Spatial advanced filter
 */
class OB_EXTENSION_API SpatialAdvancedFilter : public Filter {
public:
    SpatialAdvancedFilter();

    OBFloatPropertyRange getAlphaRange();

    OBUint16PropertyRange getDispDiffRange();

    OBUint16PropertyRange getRadiusRange();

    OBIntPropertyRange getMagnitudeRange();

    OBSpatialAdvancedFilterParams getFilterParams();

    void setFilterParams(OBSpatialAdvancedFilterParams params);
};

/**
 * @brief Depth to disparity or disparity to depth
 */
class OB_EXTENSION_API DisparityTransform : public Filter {
public:
    DisparityTransform(bool depth_to_disparity);
};

/**
 * @brief Depth HDR merge
 */
class OB_EXTENSION_API HdrMerge : public Filter {
public:
    HdrMerge();
};

/**
 * @brief Align for depth to other or other to depth.
 */
class OB_EXTENSION_API Align : public Filter {
public:
    Align(OBStreamType align_to_stream);

    OBStreamType getAlignToStreamType();
};

/**
 * @brief Creates depth Thresholding filter
 * By controlling min and max options on the block
 */
class OB_EXTENSION_API ThresholdFilter : public Filter {
public:
    ThresholdFilter();

    OBIntPropertyRange getMinRange();

    OBIntPropertyRange getMaxRange();

    void setValueRange(uint16_t min, uint16_t max);
};

/**
 * @brief
 */
class OB_EXTENSION_API SequenceIdFilter : public Filter {
public:
    SequenceIdFilter();

    void selectSequenceId(int sequence_id);

    int getSelectSequenceId();

    OBSequenceIdItem *getSequenceIdList();
    
    int getSequenceIdListSize();
};

/**
 * @brief
 */
class OB_EXTENSION_API NoiseRemovalFilter : public Filter {
public:
    NoiseRemovalFilter();

    void setFilterParams(OBNoiseRemovalFilterParams *filterParams);

    OBNoiseRemovalFilterParams getFilterParams();

    OBUint16PropertyRange getDispDiffRange();

    OBUint16PropertyRange getMaxSizeRange();
};

/**
 * @brief
 */
class OB_EXTENSION_API DecimationFilter : public Filter {
public:
    DecimationFilter();

    void setScaleValue(uint8_t value);

    uint8_t getScaleValue();

    OBUint8PropertyRange getScaleRange();
};

// Define the is() template function for the Filter class
template <typename T> bool Filter::is() {
    std::string           name         = type();
    if(name == "HdrMerge") {
        return typeid(T) == typeid(HdrMerge);
    }
    if(name == "SequenceIdFilter") {
        return typeid(T) == typeid(SequenceIdFilter);
    }
    if(name == "ThresholdFilter") {
        return typeid(T) == typeid(ThresholdFilter);
    }
    if(name == "DisparityTransform") {
        return typeid(T) == typeid(DisparityTransform);
    }
    if(name == "NoiseRemovalFilter") {
        return typeid(T) == typeid(NoiseRemovalFilter);
    }
    if(name == "SpatialAdvancedFilter") {
        return typeid(T) == typeid(SpatialAdvancedFilter);
    }
    if(name == "TemporalFilter") {
        return typeid(T) == typeid(TemporalFilter);
    }
    if(name == "HoleFillingFilter") {
        return typeid(T) == typeid(HoleFillingFilter);
    }

    return false;
}

}  // namespace ob
