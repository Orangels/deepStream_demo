//
// Created by Orangels on 2020-04-19.
//

#include "dsHandler.h"

static void cb_new_rtspsrc_pad(GstElement *element, GstPad *pad, gpointer data) {
    gchar *name;
    GstCaps *p_caps;
    gchar *description;
    GstElement *p_rtph264depay;

    name = gst_pad_get_name(pad);
    g_print("A new pad %s was created\n", name);

    // here, you would setup a new pad link for the newly created pad
    // sooo, now find that rtph264depay is needed and link them?
    p_caps = gst_pad_get_pad_template_caps(pad);

    description = gst_caps_to_string(p_caps);
    printf("%s\n", p_caps);
    printf("%s\n", description);
    g_free(description);

    p_rtph264depay = GST_ELEMENT(data);

    // try to link the pads then ...
    if (!gst_element_link_pads(element, name, p_rtph264depay, "sink")) {
        printf("Failed to link elements 3\n");
    }

    g_free(name);
}


Mat writeImage(GstBuffer *buf){
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_user_meta = NULL;
    // Get original raw data
    GstMapInfo in_map_info;
    char* src_data = NULL;
    cv::Mat out_mat;
    if (!gst_buffer_map (buf, &in_map_info, GST_MAP_READ)) {
        g_print ("Error: Failed to map gst buffer\n");
        gst_buffer_unmap (buf, &in_map_info);
//        return GST_PAD_PROBE_OK;
    }
    NvBufSurface *surface = (NvBufSurface *)in_map_info.data;

    g_print("mem type : %d\n", surface->memType);

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);
    l_frame = batch_meta->frame_meta_list;
    if (l_frame) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        /* Validate user meta */
        src_data = (char*) malloc(surface->surfaceList[frame_meta->batch_id].dataSize);
        if(src_data == NULL) {
            g_print("Error: failed to malloc src_data \n");
        }

// ls change mode

#ifdef PLATFORM_TEGRA
        NvBufSurfaceMap (surface, -1, -1, NVBUF_MAP_READ);
        NvBufSurfacePlaneParams *pParams = &surface->surfaceList[frame_meta->batch_id].planeParams;
        unsigned int offset = 0;
        for(unsigned int num_planes=0; num_planes < pParams->num_planes; num_planes++){
            if(num_planes>0)
                offset += pParams->height[num_planes-1]*(pParams->bytesPerPix[num_planes-1]*pParams->width[num_planes-1]);
            for (unsigned int h = 0; h < pParams->height[num_planes]; h++) {
                memcpy((void *)(src_data+offset+h*pParams->bytesPerPix[num_planes]*pParams->width[num_planes]),
                       (void *)((char *)surface->surfaceList[frame_meta->batch_id].mappedAddr.addr[num_planes]+h*pParams->pitch[num_planes]),
                       pParams->bytesPerPix[num_planes]*pParams->width[num_planes]
                );
            }
        }
        NvBufSurfaceSyncForDevice (surface, -1, -1);
        NvBufSurfaceUnMap (surface, -1, -1);
#else
        cudaMemcpy((void*)src_data,
                   (void*)surface->surfaceList[frame_meta->batch_id].dataPtr,
                   surface->surfaceList[frame_meta->batch_id].dataSize,
                   cudaMemcpyDeviceToHost);
#endif

        gint frame_width = (gint)surface->surfaceList[frame_meta->batch_id].width;
        gint frame_height = (gint)surface->surfaceList[frame_meta->batch_id].height;
        gint frame_step = surface->surfaceList[frame_meta->batch_id].pitch;

        cv::Mat frame = cv::Mat(frame_height, frame_width, CV_8UC4, src_data, frame_step);
        g_print("%d\n",frame.channels());
        g_print("%d\n",frame.rows);
        g_print("%d\n",frame.cols);

        string img_path;
        int frame_number = dsHandler::pic_num;
        dsHandler::pic_num++;
        img_path = "./imgs/" + to_string(frame_number) + ".jpg";

//        cv::Mat out_mat = cv::Mat (cv::Size(frame_width, frame_height), CV_8UC3);
        out_mat = cv::Mat (cv::Size(frame_width, frame_height), CV_8UC3);
        cv::cvtColor(frame, out_mat, CV_RGBA2BGR);
//        cv::imwrite(img_path, out_mat);
        if(src_data != NULL) {
            free(src_data);
            src_data = NULL;
        }
    }
    gst_buffer_unmap (buf, &in_map_info);
    return out_mat;
}

GstPadProbeReturn dsHandler::osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer u_data){
    GstBuffer *buf = (GstBuffer *) info->data;
    Mat frame = writeImage(buf);

    std::unique_lock<std::mutex> guard(myMutex);
    imgQueue.push(frame);
    con_v_notification.notify_all();
    guard.unlock();

    return GST_PAD_PROBE_OK;
}


dsHandler::dsHandler(){

}

dsHandler::dsHandler(string vRTSPCAM, int vMUXER_OUTPUT_WIDTH, int vMUXER_OUTPUT_HEIGHT, int vMUXER_BATCH_TIMEOUT_USEC) :
        RTSPCAM(vRTSPCAM), MUXER_OUTPUT_WIDTH(vMUXER_OUTPUT_WIDTH), MUXER_OUTPUT_HEIGHT(vMUXER_OUTPUT_HEIGHT),
        MUXER_BATCH_TIMEOUT_USEC(vMUXER_BATCH_TIMEOUT_USEC){

    gst_init(NULL, NULL);

    /// Build Pipeline
    pipeline = gst_pipeline_new("ls");

    /// Create elements
    source = gst_element_factory_make("rtspsrc", "source");
    g_object_set(G_OBJECT (source), "latency", 2000, NULL);
    rtppay = gst_element_factory_make("rtph264depay", "depayl");
    parse = gst_element_factory_make("h264parse", "parse");
#ifdef PLATFORM_TEGRA
    decoder = gst_element_factory_make("nvv4l2decoder", "nvv4l2-decoder");
    GstElement *streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
//    GstElement *pgie = gst_element_factory_make("nvinfer", "primary-nvinference-engine");
    GstElement *nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");
    GstElement *nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");
//    GstElement *transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
    sink = gst_element_factory_make ( "fakesink", "sink");
    if (!pipeline || !streammux  || !nvvidconv || !nvosd ) {
        g_printerr("One element could not be created. Exiting.\n");
    }
    g_object_set(G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
                 MUXER_OUTPUT_HEIGHT, "batch-size", 1,
                 "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);
//    g_object_set(G_OBJECT (pgie),
//                 "config-file-path", "../dstest1_pgie_config.txt", NULL);
#else
    decoder = gst_element_factory_make("avdec_h264", "decode");
    sink = gst_element_factory_make("appsink", "sink");
#endif
    if (!pipeline || !source || !rtppay || !parse || !decoder || !sink) {
        g_printerr("One element could not be created.\n");
    }
    g_object_set(G_OBJECT (sink), "sync", FALSE, NULL);
    g_object_set(GST_OBJECT(source), "location", RTSPCAM.c_str(), NULL);

    /// 加入插件
#ifdef PLATFORM_TEGRA
    //    gst_bin_add_many(GST_BIN (pipeline),
//                     source, rtppay, parse, decoder, streammux, pgie,
//                     nvvidconv, nvosd, transform, sink, NULL);
    gst_bin_add_many(GST_BIN (pipeline),
                     source, rtppay, parse, decoder, streammux,
                     nvvidconv, nvosd, sink, NULL);
#else
    gst_bin_add_many(GST_BIN (pipeline),
                     source, rtppay, parse, decoder, sink, NULL);
#endif
    // listen for newly created pads
    g_signal_connect(source, "pad-added", G_CALLBACK(cb_new_rtspsrc_pad), rtppay);

#ifdef PLATFORM_TEGRA
    GstPad *sinkpad, *srcpad;
    gchar pad_name_sink[16] = "sink_0";
    gchar pad_name_src[16] = "src";

    sinkpad = gst_element_get_request_pad(streammux, pad_name_sink);
    if (!sinkpad) {
        g_printerr("Streammux request sink pad failed. Exiting.\n");
    }
    //获取指定element中的指定pad  该element为 streammux
    srcpad = gst_element_get_static_pad(decoder, pad_name_src);
    if (!srcpad) {
        g_printerr("Decoder request src pad failed. Exiting.\n");
    }

    if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link decoder to stream muxer. Exiting.\n");
    }
    //gst_pad_link
    gst_object_unref(sinkpad);
    gst_object_unref(srcpad);
#endif

    /// 链接插件
#ifdef PLATFORM_TEGRA
    if (!gst_element_link_many(rtppay, parse, decoder, NULL)) {
        printf("\nFailed to link elements 0.\n");
    }
//    if (!gst_element_link_many(streammux, pgie, nvvidconv, nvosd, transform, sink, NULL)) {
    if (!gst_element_link_many(streammux, nvvidconv, nvosd, sink, NULL)) {
        printf("\nFailed to link elements 2.\n");
    }
#else
    if (!gst_element_link_many(rtppay, parse, decoder, sink, NULL)) {
        printf("\nFailed to link elements.\n");
    }
#endif

    GstPad *osd_sink_pad = NULL;
    osd_sink_pad = gst_element_get_static_pad (sink, "sink");

    if (!osd_sink_pad)
        g_print ("Unable to get sink pad\n");
    else
        gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                           osd_sink_pad_buffer_probe, NULL, NULL);
}

void dsHandler::run(){
    /// 开始运行
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                                 (GstMessageType) (GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}