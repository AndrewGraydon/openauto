/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef USE_GST

#include "aasdk/Common/Data.hpp"
#include "openauto/Projection/GSTVideoOutput.hpp"
#include "OpenautoLog.hpp"
#include "h264_stream.h"
#include <QApplication>
#include <QPainter>
#include <QScreen>

namespace openauto
{
namespace projection
{

// ---------------------------------------------------------------------------
// VideoWidget — a plain QWidget that paints decoded frames via QPainter.
// Qt stays DRM master throughout; GStreamer only decodes, never renders.
// ---------------------------------------------------------------------------

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_AcceptTouchEvents);
}

void VideoWidget::updateFrame(const QImage& frame)
{
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        currentFrame_ = frame;
    }
    update();  // schedules a paintEvent on the Qt main thread
}

void VideoWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!currentFrame_.isNull())
    {
        painter.drawImage(rect(), currentFrame_);
    }
    else
    {
        painter.fillRect(rect(), Qt::black);
    }
}

// ---------------------------------------------------------------------------
// GSTVideoOutput
// ---------------------------------------------------------------------------

GSTVideoOutput::GSTVideoOutput(configuration::IConfiguration::Pointer configuration, QWidget* videoContainer, std::function<void(bool)> activeCallback)
    : VideoOutput(std::move(configuration))
    , videoContainer_(videoContainer)
    , activeCallback_(activeCallback)
{
    this->moveToThread(QApplication::instance()->thread());

    // Create our plain Qt rendering widget
    videoWidget_ = new VideoWidget(videoContainer_);

    // Route newFrame through onFrameReady (main thread) which clears framePending_
    // before painting. This ensures at most one frame update is ever queued,
    // so touch events are not delayed behind a backlog of video frames.
    connect(this, &GSTVideoOutput::newFrame, this, &GSTVideoOutput::onFrameReady, Qt::QueuedConnection);

    // ----- Build the GStreamer pipeline -----
    // Pipeline: appsrc ! queue ! h264parse ! capssetter ! <decoder> ! videocrop ! videoconvert ! video/x-raw,format=RGB ! appsink
    // We decode to RGB so QImage can wrap the buffer directly without conversion.
    GError* error = nullptr;
    H264_Decoder decoder = findPreferredVideoDecoder();

    std::string pipelineStr =
        "appsrc name=mysrc is-live=true block=false max-latency=100 do-timestamp=true stream-type=stream "
        "! queue "
        "! h264parse "
        "! capssetter caps=\"video/x-h264,colorimetry=bt709\" "
        "! ";
    pipelineStr += ToPipeline(decoder);
    pipelineStr += " ! videocrop top=0 bottom=0 name=videocropper "
                   "! videoconvert "
                   "! video/x-raw,format=RGB "
                   "! appsink name=mysink emit-signals=true sync=false max-buffers=2 drop=true";

    vidPipeline_ = gst_parse_launch(pipelineStr.c_str(), &error);
    if (!vidPipeline_ || error)
    {
        OPENAUTO_LOG(error) << "[GSTVideoOutput] Failed to build pipeline: "
                            << (error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }

    // Bus watch for errors / EOS
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(vidPipeline_));
    gst_bus_add_watch(bus, (GstBusFunc)&GSTVideoOutput::busCallback, this);
    gst_object_unref(bus);

    // Grab appsrc
    vidSrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(vidPipeline_), "mysrc"));
    gst_app_src_set_stream_type(vidSrc_, GST_APP_STREAM_TYPE_STREAM);

    // Grab videocrop
    vidCrop_ = GST_VIDEO_FILTER(gst_bin_get_by_name(GST_BIN(vidPipeline_), "videocropper"));

    // Hook up appsink callback — delivers decoded RGB frames
    GstElement* sink = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mysink");
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &GSTVideoOutput::onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
    gst_object_unref(sink);

    connect(this, &GSTVideoOutput::startPlayback, this, &GSTVideoOutput::onStartPlayback, Qt::QueuedConnection);
    connect(this, &GSTVideoOutput::stopPlayback,  this, &GSTVideoOutput::onStopPlayback,  Qt::QueuedConnection);
}

GSTVideoOutput::~GSTVideoOutput()
{
    if (vidPipeline_)
    {
        gst_element_set_state(vidPipeline_, GST_STATE_NULL);
        gst_object_unref(vidPipeline_);
    }
    if (vidSrc_)
        gst_object_unref(vidSrc_);
}

H264_Decoder GSTVideoOutput::findPreferredVideoDecoder()
{
    for (H264_Decoder decoder : H264_Decoder_Priority_List)
    {
        GstElementFactory* factory = gst_element_factory_find(ToString(decoder));
        if (factory)
        {
            gst_object_unref(factory);
            OPENAUTO_LOG(info) << "[GSTVideoOutput] Using decoder: " << ToString(decoder);
            return decoder;
        }
    }
    OPENAUTO_LOG(error) << "[GSTVideoOutput] No supported h264 decoder found, falling back to avdec_h264";
    return H264_Decoder::libav;
}

void GSTVideoOutput::dumpDot()
{
    gst_debug_bin_to_dot_file(GST_BIN(vidPipeline_), GST_DEBUG_GRAPH_SHOW_VERBOSE, "pipeline");
    OPENAUTO_LOG(info) << "[GSTVideoOutput] Dumped pipeline dot graph";
}

gboolean GSTVideoOutput::busCallback(GstBus*, GstMessage* message, gpointer userData)
{
    auto* self = static_cast<GSTVideoOutput*>(userData);
    gchar* debug = nullptr;
    GError* err = nullptr;

    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(message, &err, &debug);
        OPENAUTO_LOG(error) << "[GSTVideoOutput] Error: " << err->message
                            << " | Debug: " << (debug ? debug : "none");
        g_error_free(err);
        g_free(debug);
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(message, &err, &debug);
        OPENAUTO_LOG(warning) << "[GSTVideoOutput] Warning: " << err->message;
        g_error_free(err);
        g_free(debug);
        break;
    case GST_MESSAGE_EOS:
        OPENAUTO_LOG(info) << "[GSTVideoOutput] End of stream";
        break;
    default:
        break;
    }

    return TRUE;
}

// Called on the GStreamer streaming thread — must be signal-safe.
GstFlowReturn GSTVideoOutput::onNewSample(GstAppSink* sink, gpointer userData)
{
    auto* self = static_cast<GSTVideoOutput*>(userData);

    GstSample* sample = gst_app_sink_pull_sample(sink);

    // If a frame is already queued on the Qt main thread, drop this one.
    // This keeps the Qt event queue shallow so touch events are not delayed
    // by a backlog of pending video frame updates.
    if (self->framePending_.exchange(true))
    {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    if (!sample)
    {
        self->framePending_.store(false);
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);

    GstVideoInfo vinfo;
    if (caps && gst_video_info_from_caps(&vinfo, caps))
    {
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            QImage frame(map.data, vinfo.width, vinfo.height,
                         vinfo.stride[0], QImage::Format_RGB888);
            emit self->newFrame(frame.copy());
            gst_buffer_unmap(buffer, &map);
        }
        else
        {
            self->framePending_.store(false);
        }
    }
    else
    {
        self->framePending_.store(false);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

bool GSTVideoOutput::open()
{
    GstElement* capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    if (capsFilter)
    {
        GstPad* pad = gst_element_get_static_pad(capsFilter, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                          &GSTVideoOutput::convertProbe, this, nullptr);
        gst_object_unref(pad);
        gst_object_unref(capsFilter);
    }

    gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);
    return true;
}

GstPadProbeReturn GSTVideoOutput::convertProbe(GstPad* pad, GstPadProbeInfo* info, void*)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
    {
        GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT)
        {
            GstCaps* caps = gst_pad_get_current_caps(pad);
            if (caps)
            {
                GstVideoInfo vinfo;
                if (gst_video_info_from_caps(&vinfo, caps))
                {
                    OPENAUTO_LOG(info) << "[GSTVideoOutput] Video: "
                                       << vinfo.width << "x" << vinfo.height;
                }
                gst_caps_unref(caps);
            }
            return GST_PAD_PROBE_REMOVE;
        }
    }
    return GST_PAD_PROBE_OK;
}

bool GSTVideoOutput::init()
{
    OPENAUTO_LOG(info) << "[GSTVideoOutput] init";
    emit startPlayback();
    return true;
}

void GSTVideoOutput::write(uint64_t /*timestamp*/, const aasdk::common::DataConstBuffer& buffer)
{
    if (!firstHeaderParsed && this->configuration_->getWhitescreenWorkaround())
    {
        // Strip video_signal_type VUI parameters from the first SPS NAL unit.
        // The Pi V4L2 h264 decoder can fail to decode frames when the phone
        // includes colour description VUI data (github.com/raspberrypi/firmware/issues/1673).

        std::vector<uint8_t> delimit_sequence{0x00, 0x00, 0x00, 0x01};
        std::vector<uint8_t> incoming_buffer(&buffer.cdata[0], &buffer.cdata[buffer.size]);

        int nal_start, nal_end;
        uint8_t* buf = (uint8_t*)buffer.cdata;
        int len = buffer.size;
        h264_stream_t* h = h264_new();
        find_nal_unit(buf, len, &nal_start, &nal_end);
        read_nal_unit(h, &buf[nal_start], nal_end - nal_start);

        h->sps->vui.video_signal_type_present_flag  = 0;
        h->sps->vui.video_format                    = 0;
        h->sps->vui.video_full_range_flag            = 0;
        h->sps->vui.colour_description_present_flag  = 0;
        h->sps->vui.colour_primaries                 = 0;
        h->sps->vui.transfer_characteristics         = 0;
        h->sps->vui.matrix_coefficients              = 0;

        uint8_t* out_buf = new uint8_t[30];
        len = write_nal_unit(h, &out_buf[3], 30) + 3;
        out_buf[0] = 0x00; out_buf[1] = 0x00; out_buf[2] = 0x00; out_buf[3] = 0x01;
        h264_free(h);

        GstBuffer* gbuf = gst_buffer_new_and_alloc(len);
        gst_buffer_fill(gbuf, 0, out_buf, len);
        delete[] out_buf;
        if (gst_app_src_push_buffer(vidSrc_, gbuf) != GST_FLOW_OK)
            OPENAUTO_LOG(warning) << "[GSTVideoOutput] Failed to push patched SPS header";

        // Forward any additional NAL units in the same message
        if (incoming_buffer.size() >= 8)
        {
            auto split = std::search(incoming_buffer.begin() + 4, incoming_buffer.end(),
                                     delimit_sequence.begin(), delimit_sequence.end());
            if (split != incoming_buffer.end())
            {
                std::vector<uint8_t> remainder(split, incoming_buffer.end());
                GstBuffer* rbuf = gst_buffer_new_and_alloc(remainder.size());
                gst_buffer_fill(rbuf, 0, remainder.data(), remainder.size());
                if (gst_app_src_push_buffer(vidSrc_, rbuf) != GST_FLOW_OK)
                    OPENAUTO_LOG(warning) << "[GSTVideoOutput] Failed to push remainder after SPS";
            }
        }

        OPENAUTO_LOG(info) << "[GSTVideoOutput] Patched VUI parameters in first SPS NAL";
        firstHeaderParsed = true;
    }
    else
    {
        GstBuffer* gbuf = gst_buffer_new_and_alloc(buffer.size);
        gst_buffer_fill(gbuf, 0, buffer.cdata, buffer.size);
        if (gst_app_src_push_buffer(vidSrc_, gbuf) != GST_FLOW_OK)
            OPENAUTO_LOG(warning) << "[GSTVideoOutput] push_buffer failed for "
                                   << buffer.size << " bytes";
    }
}

void GSTVideoOutput::onStartPlayback()
{
    firstHeaderParsed = false;

    if (activeCallback_)
        activeCallback_(true);

    if (videoContainer_ == nullptr)
    {
        QScreen* screen = QApplication::primaryScreen();
        QRect screenGeom = screen ? screen->geometry() : QRect(0, 0, 800, 480);
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Fullscreen mode: "
                           << screenGeom.width() << "x" << screenGeom.height();
        videoWidget_->setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
        videoWidget_->setGeometry(screenGeom);
        videoWidget_->show();
        videoWidget_->raise();
        videoWidget_->setFocus();
    }
    else
    {
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Resizing to container "
                           << videoContainer_->width() << "x" << videoContainer_->height();
        videoWidget_->resize(videoContainer_->size());
        videoWidget_->show();
    }

    // Dump pipeline graph after 10s for debugging
    QTimer::singleShot(10000, this, SLOT(dumpDot()));
}

void GSTVideoOutput::stop()
{
    emit stopPlayback();
}

void GSTVideoOutput::onFrameReady(const QImage& frame)
{
    // Clear the pending flag first so the GStreamer thread can queue the
    // next frame immediately, while this frame is being painted.
    framePending_.store(false);
    videoWidget_->updateFrame(frame);
}

void GSTVideoOutput::onStopPlayback()
{
    firstHeaderParsed = false;

    if (activeCallback_)
        activeCallback_(false);

    OPENAUTO_LOG(info) << "[GSTVideoOutput] stop.";
    gst_element_set_state(vidPipeline_, GST_STATE_PAUSED);
    videoWidget_->hide();
}

void GSTVideoOutput::resize()
{
    int containerWidth, containerHeight;

    if (videoContainer_)
    {
        containerWidth  = videoContainer_->width();
        containerHeight = videoContainer_->height();
        if (videoWidget_)
            videoWidget_->resize(videoContainer_->size());
    }
    else
    {
        QScreen* screen = QApplication::primaryScreen();
        QRect geom = screen ? screen->geometry() : QRect(0, 0, 800, 480);
        containerWidth  = geom.width();
        containerHeight = geom.height();
        if (videoWidget_)
            videoWidget_->setGeometry(geom);
    }

    OPENAUTO_LOG(info) << "[GSTVideoOutput] Resize to "
                       << containerWidth << "x" << containerHeight;

    int width = 800, height = 480;
    switch (this->getVideoResolution())
    {
    case aasdk::proto::enums::VideoResolution_Enum__1080p: width = 1920; height = 1080; break;
    case aasdk::proto::enums::VideoResolution_Enum__720p:  width = 1280; height = 720;  break;
    default: break;  // 480p: 800x480
    }

    double marginWidth  = 0;
    double marginHeight = 0;
    double widthRatio  = (double)containerWidth  / width;
    double heightRatio = (double)containerHeight / height;

    if (widthRatio > heightRatio)
        marginHeight = ((widthRatio * height) - containerHeight) / widthRatio / 2.0;
    else
        marginWidth  = ((heightRatio * width)  - containerWidth)  / heightRatio / 2.0;

    OPENAUTO_LOG(info) << "[GSTVideoOutput] AA " << width << "x" << height
                       << " margins: " << marginWidth << "x" << marginHeight;

    if (vidCrop_)
    {
        g_object_set(vidCrop_,
                     "top",    (int)marginHeight,
                     "bottom", (int)marginHeight,
                     "left",   (int)marginWidth,
                     "right",  (int)marginWidth,
                     nullptr);
    }

    this->configuration_->setVideoMargins(
        QRect(0, 0, (int)(marginWidth * 2), (int)(marginHeight * 2)));
}

}
}

#endif
