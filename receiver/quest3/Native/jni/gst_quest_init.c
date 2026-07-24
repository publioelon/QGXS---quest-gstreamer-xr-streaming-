#include <jni.h>
#include <android/log.h>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LOG_TAG "GstQuestInit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define DISCOVERY_REQUEST "DISCOVER_GST_QUEST_V1"
#define DISCOVERY_REPLY_PREFIX "GST_QUEST_RECEIVER_V1"
#define MAX_SIGNALING_LINE 262144


static void check_factory(const char *factory_name)
{
    GstElementFactory *factory = gst_element_factory_find(factory_name);

    if (factory != NULL) {
        LOGI("Factory found: %s", factory_name);
        gst_object_unref(factory);
    } else {
        LOGE("Factory NOT found: %s", factory_name);
    }
}


static int g_gst_initialized = 0;
static char g_version_text[128] = {0};

static pthread_t g_signaling_thread;
static int g_signaling_thread_created = 0;
static volatile int g_signaling_running = 0;
static int g_signaling_server_fd = -1;
static int g_signaling_port = 9001;

static pthread_t g_discovery_thread;
static int g_discovery_thread_created = 0;
static volatile int g_discovery_running = 0;
static int g_discovery_socket_fd = -1;
static int g_discovery_port = 9010;

static pthread_mutex_t g_message_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_last_message[1024] = {0};
static char g_last_discovery_message[1024] = {0};

static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_latest_frame_data = NULL;
static int g_latest_frame_width = 0;
static int g_latest_frame_height = 0;
static int g_latest_frame_size = 0;
static unsigned long long g_latest_frame_id = 0;

typedef struct _WebRTCSession {
    int client_fd;
    GstElement *pipeline;
    GstElement *webrtc;
    pthread_mutex_t send_mutex;
} WebRTCSession;

typedef struct _ProbeData {
    char label[128];
    unsigned long long count;
} ProbeData;


enum {
    QSXR_CODEC_AUTO = 0,
    QSXR_CODEC_H264 = 1,
    QSXR_CODEC_H265 = 2,
    QSXR_CODEC_AV1 = 3
};

static int webrtc_h264_queue_encoded_gst_buffer(GstBuffer *buffer);
static int webrtc_h265_queue_encoded_gst_buffer(GstBuffer *buffer);
static int webrtc_av1_queue_encoded_gst_buffer(GstBuffer *buffer);
static int qsxr_webrtc_video_note_incoming_codec(int codec);
static void set_last_message(const char *msg)
{
    pthread_mutex_lock(&g_message_mutex);

    if (msg == NULL) {
        g_last_message[0] = '\0';
    } else {
        snprintf(g_last_message, sizeof(g_last_message), "%s", msg);
    }

    pthread_mutex_unlock(&g_message_mutex);
}

static void set_last_discovery_message(const char *msg)
{
    pthread_mutex_lock(&g_message_mutex);

    if (msg == NULL) {
        g_last_discovery_message[0] = '\0';
    } else {
        snprintf(g_last_discovery_message, sizeof(g_last_discovery_message), "%s", msg);
    }

    pthread_mutex_unlock(&g_message_mutex);
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent = send(fd, data + sent_total, len - sent_total, 0);

        if (sent <= 0) {
            return 0;
        }

        sent_total += (size_t)sent;
    }

    return 1;
}

static int send_line_to_client(WebRTCSession *session, const char *line)
{
    if (session == NULL || line == NULL) {
        return 0;
    }

    pthread_mutex_lock(&session->send_mutex);

    int fd = session->client_fd;

    if (fd < 0) {
        pthread_mutex_unlock(&session->send_mutex);
        return 0;
    }

    char *with_newline = g_strdup_printf("%s\n", line);
    int ok = send_all(fd, with_newline, strlen(with_newline));
    g_free(with_newline);

    pthread_mutex_unlock(&session->send_mutex);

    return ok;
}

static int send_line_format(WebRTCSession *session, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char *line = g_strdup_vprintf(fmt, args);

    va_end(args);

    int ok = send_line_to_client(session, line);
    g_free(line);

    return ok;
}

static char *base64_decode_to_text(const char *encoded)
{
    if (encoded == NULL) {
        return NULL;
    }

    gsize decoded_len = 0;
    guchar *decoded = g_base64_decode(encoded, &decoded_len);

    if (decoded == NULL) {
        return NULL;
    }

    char *text = (char*)g_malloc(decoded_len + 1);
    memcpy(text, decoded, decoded_len);
    text[decoded_len] = '\0';

    g_free(decoded);

    return text;
}

static int recv_line_alloc(int fd, char *dst, int max_size)
{
    int pos = 0;

    while (g_signaling_running && pos < max_size - 1) {
        char ch = 0;
        ssize_t received = recv(fd, &ch, 1, 0);

        if (received <= 0) {
            dst[pos] = '\0';
            return received == 0 ? 0 : -1;
        }

        if (ch == '\n') {
            dst[pos] = '\0';
            return pos;
        }

        if (ch != '\r') {
            dst[pos++] = ch;
        }
    }

    dst[pos] = '\0';
    return pos;
}

static void log_element_factory_status(const char *factory_name)
{
    GstElementFactory *factory = gst_element_factory_find(factory_name);

    if (factory != NULL) {
        LOGI("Factory found: %s", factory_name);
        gst_object_unref(factory);
    } else {
        LOGE("Factory NOT found: %s", factory_name);
    }
}

static void log_matching_factories()
{
    GstRegistry *registry = gst_registry_get();
    GList *features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);

    LOGI("========== BEGIN GStreamer decoder/factory scan ==========");

    for (GList *l = features; l != NULL; l = l->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(l->data);

        const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
        const gchar *description = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);

        if (name == NULL) {
            continue;
        }

        const gchar *klass_safe = klass != NULL ? klass : "";
        const gchar *desc_safe = description != NULL ? description : "";

        gchar *name_lower = g_ascii_strdown(name, -1);
        gchar *klass_lower = g_ascii_strdown(klass_safe, -1);
        gchar *desc_lower = g_ascii_strdown(desc_safe, -1);

        int interesting =
            strstr(name_lower, "amc") != NULL ||
            strstr(name_lower, "android") != NULL ||
            strstr(name_lower, "h264") != NULL ||
            strstr(name_lower, "h265") != NULL ||
            strstr(name_lower, "avc") != NULL ||
            strstr(name_lower, "hevc") != NULL ||
            strstr(name_lower, "264") != NULL ||
            strstr(name_lower, "265") != NULL ||
            strstr(name_lower, "decoder") != NULL ||
            strstr(klass_lower, "decoder") != NULL ||
            strstr(desc_lower, "h.264") != NULL ||
            strstr(desc_lower, "h264") != NULL ||
            strstr(desc_lower, "h.265") != NULL ||
            strstr(desc_lower, "h265") != NULL ||
            strstr(desc_lower, "avc") != NULL ||
            strstr(desc_lower, "hevc") != NULL;

        if (interesting) {
            LOGI(
                "Factory scan: name=%s | klass=%s | desc=%s",
                name,
                klass_safe,
                desc_safe
            );
        }

        g_free(name_lower);
        g_free(klass_lower);
        g_free(desc_lower);
    }

    gst_plugin_feature_list_free(features);

    LOGI("========== END GStreamer decoder/factory scan ==========");
}


static void log_loaded_plugins()
{
    GstRegistry *registry = gst_registry_get();
    GList *plugins = gst_registry_get_plugin_list(registry);

    LOGI("========== BEGIN GStreamer plugin scan ==========");

    for (GList *l = plugins; l != NULL; l = l->next) {
        GstPlugin *plugin = GST_PLUGIN(l->data);

        const gchar *name = gst_plugin_get_name(plugin);
        const gchar *description = gst_plugin_get_description(plugin);
        const gchar *filename = gst_plugin_get_filename(plugin);
        const gchar *version = gst_plugin_get_version(plugin);

        const gchar *name_safe = name != NULL ? name : "";
        const gchar *desc_safe = description != NULL ? description : "";
        const gchar *file_safe = filename != NULL ? filename : "";
        const gchar *version_safe = version != NULL ? version : "";

        gchar *name_lower = g_ascii_strdown(name_safe, -1);
        gchar *desc_lower = g_ascii_strdown(desc_safe, -1);
        gchar *file_lower = g_ascii_strdown(file_safe, -1);

        int interesting =
            strstr(name_lower, "android") != NULL ||
            strstr(name_lower, "media") != NULL ||
            strstr(name_lower, "codec") != NULL ||
            strstr(name_lower, "libav") != NULL ||
            strstr(name_lower, "openh264") != NULL ||
            strstr(name_lower, "video") != NULL ||
            strstr(name_lower, "playback") != NULL ||
            strstr(desc_lower, "android") != NULL ||
            strstr(desc_lower, "media") != NULL ||
            strstr(desc_lower, "codec") != NULL ||
            strstr(desc_lower, "libav") != NULL ||
            strstr(desc_lower, "openh264") != NULL ||
            strstr(desc_lower, "video") != NULL ||
            strstr(file_lower, "android") != NULL ||
            strstr(file_lower, "media") != NULL ||
            strstr(file_lower, "codec") != NULL ||
            strstr(file_lower, "libav") != NULL ||
            strstr(file_lower, "openh264") != NULL;

        if (interesting) {
            LOGI(
                "Plugin scan: name=%s | version=%s | desc=%s | file=%s",
                name_safe,
                version_safe,
                desc_safe,
                file_safe
            );
        }

        g_free(name_lower);
        g_free(desc_lower);
        g_free(file_lower);
    }

    gst_plugin_list_free(plugins);

    LOGI("========== END GStreamer plugin scan ==========");
}

static int caps_has_encoding_name(GstCaps *caps, const char *encoding_name)
{
    if (caps == NULL || encoding_name == NULL) {
        return 0;
    }

    guint size = gst_caps_get_size(caps);

    for (guint i = 0; i < size; i++) {
        const GstStructure *structure = gst_caps_get_structure(caps, i);

        if (structure == NULL) {
            continue;
        }

        const char *value = gst_structure_get_string(structure, "encoding-name");

        if (value != NULL && g_ascii_strcasecmp(value, encoding_name) == 0) {
            return 1;
        }
    }

    return 0;
}

static void log_caps_with_label(const char *label, GstCaps *caps)
{
    if (caps == NULL) {
        LOGI("%s: caps=NULL", label);
        return;
    }

    gchar *caps_str = gst_caps_to_string(caps);

    if (caps_str != NULL) {
        LOGI("%s: %s", label, caps_str);
        g_free(caps_str);
    } else {
        LOGI("%s: could not convert caps to string", label);
    }
}

static GstPadProbeReturn on_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    ProbeData *data = (ProbeData*)user_data;

    if (data == NULL) {
        return GST_PAD_PROBE_OK;
    }

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        data->count++;

        if (data->count == 1 || data->count % 900 == 0) {
            GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);

            if (buffer != NULL) {
                LOGI(
                    "[probe] %s buffer #%llu size=%lu",
                    data->label,
                    data->count,
                    (unsigned long)gst_buffer_get_size(buffer)
                );
            } else {
                LOGI("[probe] %s buffer #%llu", data->label, data->count);
            }
        }
    }

    return GST_PAD_PROBE_OK;
}

static void probe_data_destroy(gpointer user_data)
{
    ProbeData *data = (ProbeData*)user_data;

    if (data != NULL) {
        g_free(data);
    }
}

static void add_buffer_probe(GstElement *element, const char *pad_name, const char *label)
{
    /* Phase 1 H.265 software stability: do not attach verbose RTP/depay/parse probes
     * to the H.265 path during normal playback. The logs showed thousands of
     * probe messages while video was already decoding/uploading correctly, and
     * Android logcat pressure can contribute to stutter on Quest.
     * H.264 remains unchanged.
     */
    if (label != NULL && (strstr(label, "H.265") != NULL || strstr(label, "h265") != NULL)) {
        return;
    }
    if (element == NULL || pad_name == NULL || label == NULL) {
        return;
    }

    GstPad *pad = gst_element_get_static_pad(element, pad_name);

    if (pad == NULL) {
        LOGE("Could not add probe. Element pad not found: %s:%s", GST_ELEMENT_NAME(element), pad_name);
        return;
    }

    ProbeData *data = (ProbeData*)g_malloc0(sizeof(ProbeData));
    snprintf(data->label, sizeof(data->label), "%s", label);
    data->count = 0;

    gst_pad_add_probe(
        pad,
        GST_PAD_PROBE_TYPE_BUFFER,
        on_buffer_probe,
        data,
        probe_data_destroy
    );

    LOGI("Added buffer probe on %s:%s as %s", GST_ELEMENT_NAME(element), pad_name, label);

    gst_object_unref(pad);
}

static void poll_pipeline_bus(WebRTCSession *session)
{
    if (session == NULL || session->pipeline == NULL) {
        return;
    }

    GstBus *bus = gst_element_get_bus(session->pipeline);

    if (bus == NULL) {
        return;
    }

    while (1) {
        GstMessage *msg = gst_bus_pop(bus);

        if (msg == NULL) {
            break;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError *err = NULL;
                gchar *debug = NULL;

                gst_message_parse_error(msg, &err, &debug);

                LOGE(
                    "GStreamer ERROR from %s: %s | debug=%s",
                    GST_OBJECT_NAME(msg->src),
                    err != NULL ? err->message : "unknown",
                    debug != NULL ? debug : "none"
                );

                if (err != NULL) {
                    g_error_free(err);
                }

                if (debug != NULL) {
                    g_free(debug);
                }

                break;
            }

            case GST_MESSAGE_WARNING: {
                GError *err = NULL;
                gchar *debug = NULL;

                gst_message_parse_warning(msg, &err, &debug);

                LOGE(
                    "GStreamer WARNING from %s: %s | debug=%s",
                    GST_OBJECT_NAME(msg->src),
                    err != NULL ? err->message : "unknown",
                    debug != NULL ? debug : "none"
                );

                if (err != NULL) {
                    g_error_free(err);
                }

                if (debug != NULL) {
                    g_free(debug);
                }

                break;
            }

            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(session->pipeline)) {
                    GstState old_state;
                    GstState new_state;
                    GstState pending_state;

                    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

                    LOGI(
                        "Pipeline state changed: %s -> %s",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state)
                    );
                }

                break;
            }

            case GST_MESSAGE_EOS:
                LOGI("GStreamer EOS received");
                break;

            default:
                break;
        }

        gst_message_unref(msg);
    }

    gst_object_unref(bus);
}

static void store_latest_frame(GstSample *sample)
{
    if (sample == NULL) {
        return;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    if (buffer == NULL || caps == NULL) {
        return;
    }

    GstStructure *structure = gst_caps_get_structure(caps, 0);

    if (structure == NULL) {
        return;
    }

    int width = 0;
    int height = 0;

    if (!gst_structure_get_int(structure, "width", &width)) {
        return;
    }

    if (!gst_structure_get_int(structure, "height", &height)) {
        return;
    }

    const char *format = gst_structure_get_string(structure, "format");

    if (format == NULL || strcmp(format, "RGBA") != 0) {
        return;
    }

    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return;
    }

    if (map.data == NULL || map.size == 0 || map.size > INT_MAX) {
        gst_buffer_unmap(buffer, &map);
        return;
    }

    pthread_mutex_lock(&g_frame_mutex);

    if (g_latest_frame_data == NULL || g_latest_frame_size != (int)map.size) {
        unsigned char *new_buffer = (unsigned char*)realloc(g_latest_frame_data, map.size);

        if (new_buffer == NULL) {
            pthread_mutex_unlock(&g_frame_mutex);
            gst_buffer_unmap(buffer, &map);
            return;
        }

        g_latest_frame_data = new_buffer;
    }

    memcpy(g_latest_frame_data, map.data, map.size);
    g_latest_frame_width = width;
    g_latest_frame_height = height;
    g_latest_frame_size = (int)map.size;
    g_latest_frame_id++;

    pthread_mutex_unlock(&g_frame_mutex);

    gst_buffer_unmap(buffer, &map);
}

JNIEXPORT int JNICALL gst_quest_get_latest_frame_width()
{
    pthread_mutex_lock(&g_frame_mutex);
    int value = g_latest_frame_width;
    pthread_mutex_unlock(&g_frame_mutex);
    return value;
}

JNIEXPORT int JNICALL gst_quest_get_latest_frame_height()
{
    pthread_mutex_lock(&g_frame_mutex);
    int value = g_latest_frame_height;
    pthread_mutex_unlock(&g_frame_mutex);
    return value;
}

JNIEXPORT int JNICALL gst_quest_get_latest_frame_size()
{
    pthread_mutex_lock(&g_frame_mutex);
    int value = g_latest_frame_size;
    pthread_mutex_unlock(&g_frame_mutex);
    return value;
}

JNIEXPORT unsigned long long JNICALL gst_quest_get_latest_frame_id()
{
    pthread_mutex_lock(&g_frame_mutex);
    unsigned long long value = g_latest_frame_id;
    pthread_mutex_unlock(&g_frame_mutex);
    return value;
}

JNIEXPORT int JNICALL gst_quest_copy_latest_frame(unsigned char *dst, int dst_size)
{
    if (dst == NULL || dst_size <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_frame_mutex);

    if (g_latest_frame_data == NULL || g_latest_frame_size <= 0) {
        pthread_mutex_unlock(&g_frame_mutex);
        return 0;
    }

    if (dst_size < g_latest_frame_size) {
        int needed = g_latest_frame_size;
        pthread_mutex_unlock(&g_frame_mutex);
        return -needed;
    }

    memcpy(dst, g_latest_frame_data, g_latest_frame_size);
    int copied = g_latest_frame_size;

    pthread_mutex_unlock(&g_frame_mutex);

    return copied;
}

JNIEXPORT void JNICALL gst_quest_clear_latest_frame()
{
    pthread_mutex_lock(&g_frame_mutex);

    if (g_latest_frame_data != NULL) {
        free(g_latest_frame_data);
        g_latest_frame_data = NULL;
    }

    g_latest_frame_width = 0;
    g_latest_frame_height = 0;
    g_latest_frame_size = 0;
    g_latest_frame_id = 0;

    pthread_mutex_unlock(&g_frame_mutex);
}

static JavaVM *g_quest_java_vm = NULL;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_quest_java_vm = vm;
    __android_log_print(ANDROID_LOG_INFO, "GstQuestInit", "Stored JavaVM for OES bridge");
    LOGI("JNI_OnLoad called for GstQuestInit");
    return JNI_VERSION_1_6;
}

JNIEXPORT int JNICALL gst_quest_init()
{
    if (g_gst_initialized) {
        LOGI("GStreamer already initialized");
        return 1;
    }

    int argc = 0;
    char **argv = NULL;

    GError *error = NULL;
    gboolean ok = gst_init_check(&argc, &argv, &error);

    if (!ok) {
        if (error != NULL) {
            LOGE("gst_init_check failed: %s", error->message);
            g_error_free(error);
        } else {
            LOGE("gst_init_check failed with unknown error");
        }

        return 0;
    }

    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;

    gst_version(&major, &minor, &micro, &nano);

    snprintf(
        g_version_text,
        sizeof(g_version_text),
        "GStreamer %u.%u.%u.%u",
        major,
        minor,
        micro,
        nano
    );

    LOGI("GStreamer initialized successfully: %s", g_version_text);

    LOGI("Checking important GStreamer factories...");

    /* AV1 software/RTP receiver capability checks.
     * Phase AV1-0 only logs availability. It does not change H.264/H.265 behavior.
     */
    check_factory("rtpav1depay");
    check_factory("av1parse");
    check_factory("avdec_av1");
    check_factory("dav1ddec");
    log_element_factory_status("webrtcbin");
    log_element_factory_status("queue");
    log_element_factory_status("rtph264depay");
    log_element_factory_status("h264parse");
    log_element_factory_status("rtph265depay");
    log_element_factory_status("h265parse");
    log_element_factory_status("decodebin");
    log_element_factory_status("videoconvert");
    log_element_factory_status("appsink");
    log_element_factory_status("avdec_h264");
    log_element_factory_status("openh264dec");
    log_element_factory_status("avdec_h265");
    log_element_factory_status("androidmedia");
    log_element_factory_status("amcviddec");
    log_element_factory_status("mediacodec");
    log_element_factory_status("amcviddec-omxgoogleh264decoder");
    log_element_factory_status("amcviddec-c2androidavcdecoder");
    log_element_factory_status("amcviddec-c2qtiavcdecoder");
    log_element_factory_status("amcviddec-c2qtiavcdecoderlowlatency");
    log_element_factory_status("amcviddec-c2qtihevcdecoder");
    log_element_factory_status("amcviddec-c2qtihevcdecoderlowlatency");
    log_element_factory_status("amcviddec-c2androidhevcdecoder");
    log_element_factory_status("amcviddec-omxqcomvideodecoderhevc");
    log_element_factory_status("amcviddec-omxqcomvideodecoderhevclowlatency");
    log_element_factory_status("amcviddec-omxqcomvideodecoderavc");
    log_element_factory_status("amcviddec-omxqcomvideodecoderavclowlatency");
    log_matching_factories();
    log_loaded_plugins();

    g_gst_initialized = 1;
    return 1;
}

JNIEXPORT const char* JNICALL gst_quest_version()
{
    if (g_version_text[0] == '\0') {
        snprintf(g_version_text, sizeof(g_version_text), "GStreamer not initialized");
    }

    return g_version_text;
}

JNIEXPORT int JNICALL gst_quest_has_webrtcbin()
{
    if (!g_gst_initialized) {
        LOGI("GStreamer was not initialized yet. Initializing now before checking webrtcbin.");

        if (!gst_quest_init()) {
            LOGE("Cannot check webrtcbin because GStreamer initialization failed.");
            return 0;
        }
    }

    GstElementFactory *factory = gst_element_factory_find("webrtcbin");

    if (factory != NULL) {
        LOGI("webrtcbin found");
        gst_object_unref(factory);
        return 1;
    }

    LOGE("webrtcbin NOT found");
    return 0;
}

static GstFlowReturn on_decoded_sample(GstElement *appsink, gpointer user_data)
{
    static int frame_count = 0;

    GstSample *sample = NULL;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);

    if (sample == NULL) {
        LOGE("appsink new-sample signal fired, but pull-sample returned NULL");
        return GST_FLOW_OK;
    }

    store_latest_frame(sample);

    frame_count++;

    if (frame_count == 1 || frame_count % 30 == 0) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

        if (caps != NULL) {
            gchar *caps_str = gst_caps_to_string(caps);
            LOGI("appsink received decoded frame %d | caps=%s", frame_count, caps_str);
            g_free(caps_str);
        } else {
            LOGI("appsink received decoded frame %d | caps=NULL", frame_count);
        }

        if (buffer != NULL) {
            LOGI(
                "decoded frame %d buffer size=%lu latest_frame_id=%llu latest_frame_size=%d",
                frame_count,
                (unsigned long)gst_buffer_get_size(buffer),
                g_latest_frame_id,
                g_latest_frame_size
            );
        } else {
            LOGI("decoded frame %d buffer=NULL", frame_count);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static int h265_annexb_buffer_has_vcl(GstBuffer *buffer, unsigned int *first_vcl_type, unsigned int *nal_count_out)
{
    if (first_vcl_type != NULL) {
        *first_vcl_type = 999;
    }

    if (nal_count_out != NULL) {
        *nal_count_out = 0;
    }

    if (buffer == NULL) {
        return 0;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return 0;
    }

    const unsigned char *data = (const unsigned char*)map.data;
    size_t size = (size_t)map.size;
    int has_vcl = 0;
    unsigned int nal_count = 0;

    size_t i = 0;
    while (i + 5 < size) {
        size_t sc_pos = (size_t)-1;
        size_t sc_len = 0;

        for (; i + 3 < size; i++) {
            if (i + 4 <= size &&
                data[i] == 0x00 && data[i + 1] == 0x00 &&
                data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                sc_pos = i;
                sc_len = 4;
                break;
            }

            if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
                sc_pos = i;
                sc_len = 3;
                break;
            }
        }

        if (sc_pos == (size_t)-1) {
            break;
        }

        size_t nal_start = sc_pos + sc_len;
        if (nal_start + 2 > size) {
            break;
        }

        unsigned int nal_type = (data[nal_start] >> 1) & 0x3F;
        nal_count++;

        if (nal_type <= 31) {
            has_vcl = 1;
            if (first_vcl_type != NULL) {
                *first_vcl_type = nal_type;
            }
            break;
        }

        i = nal_start + 2;
    }

    if (nal_count_out != NULL) {
        *nal_count_out = nal_count;
    }

    gst_buffer_unmap(buffer, &map);
    return has_vcl;
}


static GstFlowReturn on_encoded_h264_sample(GstElement *appsink, gpointer user_data)
{
    (void)user_data;

    static unsigned long long au_count = 0;
    static gint64 start_us = 0;

    GstSample *sample = NULL;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);

    if (sample == NULL) {
        LOGE("WebRTC H.264 encoded appsink new-sample fired, but pull-sample returned NULL");
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    au_count++;

    if (buffer != NULL) {
        webrtc_h264_queue_encoded_gst_buffer(buffer);
    } else {
        LOGE("WebRTC H.264 encoded sample did not contain a GstBuffer");
    }

    gint64 now_us = g_get_monotonic_time();
    if (start_us == 0) start_us = now_us;

    double elapsed_s = (double)(now_us - start_us) / 1000000.0;
    double avg_rate = elapsed_s > 0.0 ? ((double)au_count / elapsed_s) : 0.0;

    if (au_count == 1 || au_count % 120 == 0) {
        unsigned long size = buffer != NULL
            ? (unsigned long)gst_buffer_get_size(buffer)
            : 0;
        gchar *caps_str = caps != NULL ? gst_caps_to_string(caps) : NULL;

        LOGI(
            "WebRTC H.264 encoded AU appsink: au=%llu avg_rate=%.2f buffer_size=%lu caps=%s",
            au_count,
            avg_rate,
            size,
            caps_str != NULL ? caps_str : "NULL"
        );

        if (caps_str != NULL) g_free(caps_str);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn on_encoded_h265_sample(GstElement *appsink, gpointer user_data)
{
    static unsigned long long au_count = 0;
    static gint64 start_us = 0;

    GstSample *sample = NULL;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);

    if (sample == NULL) {
        LOGE("WebRTC H.265 encoded appsink new-sample fired, but pull-sample returned NULL");
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    

    static unsigned long long skipped_non_vcl = 0;

    if (buffer != NULL) {
        unsigned int first_vcl_type = 999;
        unsigned int nal_count = 0;
        unsigned long size = (unsigned long)gst_buffer_get_size(buffer);

        int has_vcl = h265_annexb_buffer_has_vcl(buffer, &first_vcl_type, &nal_count);

        if (has_vcl) {
            webrtc_h265_queue_encoded_gst_buffer(buffer);
        } else {
            skipped_non_vcl++;

            if (skipped_non_vcl <= 20 || skipped_non_vcl % 120 == 0 || size <= 64) {
                LOGI(
                    "WebRTC H.265 skip non-VCL buffer: skipped=%llu size=%lu nal_count=%u",
                    skipped_non_vcl,
                    size,
                    nal_count
                );
            }
        }
    }
GstCaps *caps = gst_sample_get_caps(sample);

    au_count++;

    gint64 now_us = g_get_monotonic_time();
    if (start_us == 0) {
        start_us = now_us;
    }

    double elapsed_s = (double)(now_us - start_us) / 1000000.0;
    double avg_fps = elapsed_s > 0.0 ? ((double)au_count / elapsed_s) : 0.0;

    if (au_count == 1 || au_count % 120 == 0) {
        unsigned long size = buffer != NULL ? (unsigned long)gst_buffer_get_size(buffer) : 0;
        gchar *caps_str = caps != NULL ? gst_caps_to_string(caps) : NULL;

        LOGI(
            "WebRTC H.265 encoded AU appsink: au=%llu avg_fps=%.2f buffer_size=%lu caps=%s",
            au_count,
            avg_fps,
            size,
            caps_str != NULL ? caps_str : "NULL"
        );

        if (caps_str != NULL) {
            g_free(caps_str);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn on_encoded_av1_sample(GstElement *appsink, gpointer user_data)
{
    (void)user_data;

    static unsigned long long unit_count = 0;
    static gint64 start_us = 0;

    GstSample *sample = NULL;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);

    if (sample == NULL) {
        LOGE("WebRTC AV1 encoded appsink new-sample fired, but pull-sample returned NULL");
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    unit_count++;

    if (buffer != NULL) {
        webrtc_av1_queue_encoded_gst_buffer(buffer);
    } else {
        LOGE("WebRTC AV1 encoded sample did not contain a GstBuffer");
    }

    gint64 now_us = g_get_monotonic_time();

    if (start_us == 0) {
        start_us = now_us;
    }

    double elapsed_s = (double)(now_us - start_us) / 1000000.0;
    double avg_rate = elapsed_s > 0.0 ? ((double)unit_count / elapsed_s) : 0.0;

    if (unit_count == 1 || unit_count % 120 == 0) {
        unsigned long size =
            buffer != NULL ? (unsigned long)gst_buffer_get_size(buffer) : 0;

        gchar *caps_str = caps != NULL ? gst_caps_to_string(caps) : NULL;

        LOGI(
            "WebRTC AV1 encoded temporal-unit appsink: unit=%llu avg_rate=%.2f buffer_size=%lu caps=%s",
            unit_count,
            avg_rate,
            size,
            caps_str != NULL ? caps_str : "NULL"
        );

        if (caps_str != NULL) {
            g_free(caps_str);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void on_decodebin_pad_added(GstElement *decodebin, GstPad *decoded_pad, gpointer user_data)
{
    GstElement *convert = GST_ELEMENT(user_data);

    if (convert == NULL) {
        LOGE("decodebin pad-added: convert is NULL");
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(decoded_pad);

    if (caps == NULL) {
        caps = gst_pad_query_caps(decoded_pad, NULL);
    }

    log_caps_with_label("decodebin produced pad caps", caps);

    if (caps != NULL) {
        gst_caps_unref(caps);
    }

    GstPad *convert_sink_pad = gst_element_get_static_pad(convert, "sink");

    if (convert_sink_pad == NULL) {
        LOGE("decodebin pad-added: could not get videoconvert sink pad");
        return;
    }

    if (gst_pad_is_linked(convert_sink_pad)) {
        LOGI("decodebin pad-added: videoconvert sink pad already linked");
        gst_object_unref(convert_sink_pad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(decoded_pad, convert_sink_pad);
    gst_object_unref(convert_sink_pad);

    if (ret != GST_PAD_LINK_OK) {
        LOGE("Could not link decodebin output to videoconvert. ret=%d", ret);
        return;
    }

    LOGI("decodebin linked to videoconvert");
}


static GstElement *create_h264_decoder_with_fallback(const gchar **selected_factory_name)
{
    static const gchar *decoder_candidates[] = {
        "amcviddec-c2qtiavcdecoderlowlatency",
        "amcviddec-c2qtiavcdecoder",
        "amcviddec-omxqcomvideodecoderavclowlatency",
        "amcviddec-omxqcomvideodecoderavc",
        "avdec_h264",
        "openh264dec",
        NULL
    };

    for (int i = 0; decoder_candidates[i] != NULL; i++) {
        const gchar *factory_name = decoder_candidates[i];
        GstElementFactory *factory = gst_element_factory_find(factory_name);

        if (factory == NULL) {
            LOGI("H.264 decoder candidate not available: %s", factory_name);
            continue;
        }

        gst_object_unref(factory);

        GstElement *decoder = gst_element_factory_make(factory_name, "h264_decoder");

        if (decoder == NULL) {
            LOGE("H.264 decoder candidate factory exists but element creation failed: %s", factory_name);
            continue;
        }

        if (selected_factory_name != NULL) {
            *selected_factory_name = factory_name;
        }

        LOGI("Selected H.264 decoder factory: %s", factory_name);
        return decoder;
    }

    if (selected_factory_name != NULL) {
        *selected_factory_name = NULL;
    }

    LOGE("No usable H.264 decoder factory could be created.");
    return NULL;
}

static GstElement *create_h265_decoder_with_fallback(const gchar **selected_factory_name)
{
    static const gchar *decoder_candidates[] = {
        "avdec_h265",
        NULL
    };

    for (int i = 0; decoder_candidates[i] != NULL; i++) {
        const gchar *factory_name = decoder_candidates[i];
        GstElementFactory *factory = gst_element_factory_find(factory_name);

        if (factory == NULL) {
            LOGI("H.265 decoder candidate not available: %s", factory_name);
            continue;
        }

        gst_object_unref(factory);

        GstElement *decoder = gst_element_factory_make(factory_name, "h265_decoder");

        if (decoder == NULL) {
            LOGE("H.265 decoder candidate factory exists but element creation failed: %s", factory_name);
            continue;
        }

        if (selected_factory_name != NULL) {
            *selected_factory_name = factory_name;
        }

        LOGI("Selected H.265 decoder factory: %s", factory_name);
        return decoder;
    }

    if (selected_factory_name != NULL) {
        *selected_factory_name = NULL;
    }

    LOGE("No usable H.265 decoder factory could be created.");
    return NULL;
}

static GstElement *create_av1_decoder_with_fallback(const gchar **selected_factory_name)
{
    static const gchar *decoder_candidates[] = {
        "dav1ddec",
        "avdec_av1",
        NULL
    };

    for (int i = 0; decoder_candidates[i] != NULL; i++) {
        const gchar *factory_name = decoder_candidates[i];
        GstElementFactory *factory = gst_element_factory_find(factory_name);

        if (factory == NULL) {
            LOGI("AV1 decoder candidate not available: %s", factory_name);
            continue;
        }

        gst_object_unref(factory);

        GstElement *decoder = gst_element_factory_make(factory_name, "av1_decoder");

        if (decoder == NULL) {
            LOGE("AV1 decoder candidate factory exists but element creation failed: %s", factory_name);
            continue;
        }

        if (selected_factory_name != NULL) {
            *selected_factory_name = factory_name;
        }

        LOGI("Selected AV1 decoder factory: %s", factory_name);
        return decoder;
    }

    if (selected_factory_name != NULL) {
        *selected_factory_name = NULL;
    }

    LOGE("No usable AV1 decoder factory could be created.");
    return NULL;
}

static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data)
{
    WebRTCSession *session = (WebRTCSession*)user_data;

    if (session == NULL || session->pipeline == NULL) {
        LOGE("Incoming WebRTC stream ignored because session or pipeline is NULL");
        return;
    }

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) {
        LOGI("Ignoring non-src WebRTC pad");
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);

    if (caps == NULL) {
        caps = gst_pad_query_caps(pad, NULL);
    }

    log_caps_with_label("Incoming WebRTC pad caps", caps);

    int is_h264 = 0;
    int is_h265 = 0;

        gboolean is_av1 = FALSE;
if (caps != NULL) {
        is_h264 = caps_has_encoding_name(caps, "H264");
        is_h265 = caps_has_encoding_name(caps, "H265");
        is_av1 = caps_has_encoding_name(caps, "AV1");
    }

    if (!is_h264 && !is_h265 && !is_av1) {
        LOGE("Incoming WebRTC pad is not H.264, H.265, or AV1, or caps did not expose encoding-name=H264/H265/AV1.");
        if (caps != NULL) {
            gst_caps_unref(caps);
        }
        return;
    }

    if (caps != NULL) {
        gst_caps_unref(caps);
    }

    int incoming_codec = is_h264
        ? QSXR_CODEC_H264
        : (is_h265 ? QSXR_CODEC_H265 : QSXR_CODEC_AV1);

    if (!qsxr_webrtc_video_note_incoming_codec(incoming_codec)) {
        LOGE(
            "Incoming codec %d was rejected by the configured QSXR codec policy.",
            incoming_codec
        );
        return;
    }

    if (is_av1) {
        LOGI("Incoming stream detected as AV1. Building AV1 encoded temporal-unit appsink chain.");

        GstElement *queue =
            gst_element_factory_make("queue", "av1_webrtc_encoded_queue");

        GstElement *depay =
            gst_element_factory_make("rtpav1depay", "av1_webrtc_depay");

        GstElement *parse =
            gst_element_factory_make("av1parse", "av1_webrtc_parse");

        GstElement *capsfilter =
            gst_element_factory_make("capsfilter", "av1_webrtc_encoded_caps");

        GstElement *sink =
            gst_element_factory_make("appsink", "av1_webrtc_encoded_sink");

        if (
            queue == NULL ||
            depay == NULL ||
            parse == NULL ||
            capsfilter == NULL ||
            sink == NULL
        ) {
            LOGE("Could not create one or more AV1 WebRTC encoded appsink elements");

            LOGE(
                "queue=%p depay=%p parse=%p capsfilter=%p appsink=%p",
                queue,
                depay,
                parse,
                capsfilter,
                sink
            );

            if (queue != NULL) gst_object_unref(queue);
            if (depay != NULL) gst_object_unref(depay);
            if (parse != NULL) gst_object_unref(parse);
            if (capsfilter != NULL) gst_object_unref(capsfilter);
            if (sink != NULL) gst_object_unref(sink);

            return;
        }

        g_object_set(
            queue,
            "max-size-buffers", 512,
            "max-size-bytes", 0,
            "max-size-time", 0,
            "leaky", 0,
            NULL
        );

        g_object_set(
            depay,
            "request-keyframe", TRUE,
            "wait-for-keyframe", TRUE,
            NULL
        );

        GstCaps *encoded_caps = gst_caps_from_string(
            "video/x-av1,"
            "parsed=(boolean)true,"
            "stream-format=(string)obu-stream,"
            "alignment=(string)tu"
        );

        if (encoded_caps == NULL) {
            LOGE("Could not create AV1 encoded temporal-unit caps");

            gst_object_unref(queue);
            gst_object_unref(depay);
            gst_object_unref(parse);
            gst_object_unref(capsfilter);
            gst_object_unref(sink);

            return;
        }

        g_object_set(
            capsfilter,
            "caps", encoded_caps,
            NULL
        );

        g_object_set(
            sink,
            "caps", encoded_caps,
            "emit-signals", TRUE,
            "sync", FALSE,
            "max-buffers", 240,
            "drop", FALSE,
            "enable-last-sample", FALSE,
            NULL
        );

        gst_caps_unref(encoded_caps);

        g_signal_connect(
            sink,
            "new-sample",
            G_CALLBACK(on_encoded_av1_sample),
            session
        );

        gst_bin_add_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            capsfilter,
            sink,
            NULL
        );

        if (!gst_element_link_many(
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            )) {
            LOGE(
                "Could not link AV1 WebRTC encoded chain: "
                "queue -> rtpav1depay -> av1parse -> capsfilter -> appsink"
            );

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        GstPad *queue_sink_pad =
            gst_element_get_static_pad(queue, "sink");

        if (queue_sink_pad == NULL) {
            LOGE("Could not get AV1 WebRTC encoded queue sink pad");

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
        gst_object_unref(queue_sink_pad);

        if (ret != GST_PAD_LINK_OK) {
            LOGE(
                "Could not link incoming WebRTC pad to AV1 encoded queue. ret=%d",
                ret
            );

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(capsfilter);
        gst_element_sync_state_with_parent(sink);

        LOGI(
            "Incoming WebRTC media linked to AV1 ENCODED temporal-unit appsink pipeline."
        );

        return;
    }

    if (is_h265) {
        LOGI("Incoming stream detected as H.265. Building H.265 encoded access-unit appsink chain.");

        GstElement *queue = gst_element_factory_make("queue", "h265_webrtc_encoded_queue");
        GstElement *depay = gst_element_factory_make("rtph265depay", "h265_webrtc_depay");
        GstElement *parse = gst_element_factory_make("h265parse", "h265_webrtc_parse");
        GstElement *capsfilter = gst_element_factory_make("capsfilter", "h265_webrtc_encoded_caps");
        GstElement *sink = gst_element_factory_make("appsink", "h265_webrtc_encoded_sink");

        if (queue == NULL || depay == NULL || parse == NULL || capsfilter == NULL || sink == NULL) {
            LOGE("Could not create one or more H.265 WebRTC encoded appsink elements");
            LOGE(
                "queue=%p depay=%p parse=%p capsfilter=%p appsink=%p",
                queue,
                depay,
                parse,
                capsfilter,
                sink
            );

            if (queue != NULL) gst_object_unref(queue);
            if (depay != NULL) gst_object_unref(depay);
            if (parse != NULL) gst_object_unref(parse);
            if (capsfilter != NULL) gst_object_unref(capsfilter);
            if (sink != NULL) gst_object_unref(sink);

            return;
        }

        g_object_set(
            queue,
            "max-size-buffers", 512,
            "max-size-bytes", 0,
            "max-size-time", 0,
            "leaky", 0,
            NULL
        );

        g_object_set(
            depay,
            "request-keyframe", FALSE,
            "wait-for-keyframe", FALSE,
            NULL
        );

        g_object_set(
            parse,
            "config-interval", -1,
            NULL
        );

        GstCaps *encoded_caps = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");

        g_object_set(
            capsfilter,
            "caps", encoded_caps,
            NULL
        );

        g_object_set(
            sink,
            "caps", encoded_caps,
            "emit-signals", TRUE,
            "sync", FALSE,
            "max-buffers", 240,
            "drop", FALSE,
            "enable-last-sample", FALSE,
            NULL
        );

        if (encoded_caps != NULL) {
            gst_caps_unref(encoded_caps);
        }

        g_signal_connect(sink, "new-sample", G_CALLBACK(on_encoded_h265_sample), session);

        gst_bin_add_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            capsfilter,
            sink,
            NULL
        );

        if (!gst_element_link_many(queue, depay, parse, capsfilter, sink, NULL)) {
            LOGE("Could not link H.265 WebRTC encoded chain: queue -> rtph265depay -> h265parse -> capsfilter -> appsink");

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        GstPad *queue_sink_pad = gst_element_get_static_pad(queue, "sink");

        if (queue_sink_pad == NULL) {
            LOGE("Could not get H.265 WebRTC encoded queue sink pad");

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
        gst_object_unref(queue_sink_pad);

        if (ret != GST_PAD_LINK_OK) {
            LOGE("Could not link incoming WebRTC pad to H.265 encoded queue. ret=%d", ret);

            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                capsfilter,
                sink,
                NULL
            );

            return;
        }

        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(capsfilter);
        gst_element_sync_state_with_parent(sink);

        LOGI("Incoming WebRTC media linked to H.265 ENCODED access-unit appsink pipeline.");

        return;
    }
    LOGI("Incoming stream detected as H.264. Building H.264 encoded access-unit appsink chain.");

    GstElement *queue =
        gst_element_factory_make("queue", "h264_webrtc_encoded_queue");
    GstElement *depay =
        gst_element_factory_make("rtph264depay", "h264_webrtc_depay");
    GstElement *parse =
        gst_element_factory_make("h264parse", "h264_webrtc_parse");
    GstElement *capsfilter =
        gst_element_factory_make("capsfilter", "h264_webrtc_encoded_caps");
    GstElement *sink =
        gst_element_factory_make("appsink", "h264_webrtc_encoded_sink");

    if (
        queue == NULL || depay == NULL || parse == NULL ||
        capsfilter == NULL || sink == NULL
    ) {
        LOGE("Could not create one or more H.264 WebRTC encoded appsink elements");

        if (queue != NULL) gst_object_unref(queue);
        if (depay != NULL) gst_object_unref(depay);
        if (parse != NULL) gst_object_unref(parse);
        if (capsfilter != NULL) gst_object_unref(capsfilter);
        if (sink != NULL) gst_object_unref(sink);
        return;
    }

    g_object_set(
        queue,
        "max-size-buffers", 512,
        "max-size-bytes", 0,
        "max-size-time", 0,
        "leaky", 0,
        NULL
    );

    g_object_set(
        depay,
        "request-keyframe", TRUE,
        "wait-for-keyframe", TRUE,
        NULL
    );

    g_object_set(parse, "config-interval", -1, NULL);

    GstCaps *encoded_caps = gst_caps_from_string(
        "video/x-h264,"
        "parsed=(boolean)true,"
        "stream-format=(string)byte-stream,"
        "alignment=(string)au"
    );

    if (encoded_caps == NULL) {
        LOGE("Could not create H.264 encoded access-unit caps");
        gst_object_unref(queue);
        gst_object_unref(depay);
        gst_object_unref(parse);
        gst_object_unref(capsfilter);
        gst_object_unref(sink);
        return;
    }

    g_object_set(capsfilter, "caps", encoded_caps, NULL);
    g_object_set(
        sink,
        "caps", encoded_caps,
        "emit-signals", TRUE,
        "sync", FALSE,
        "max-buffers", 240,
        "drop", FALSE,
        "enable-last-sample", FALSE,
        NULL
    );
    gst_caps_unref(encoded_caps);

    g_signal_connect(
        sink,
        "new-sample",
        G_CALLBACK(on_encoded_h264_sample),
        session
    );

    gst_bin_add_many(
        GST_BIN(session->pipeline),
        queue,
        depay,
        parse,
        capsfilter,
        sink,
        NULL
    );

    if (!gst_element_link_many(queue, depay, parse, capsfilter, sink, NULL)) {
        LOGE(
            "Could not link H.264 WebRTC encoded chain: "
            "queue -> rtph264depay -> h264parse -> capsfilter -> appsink"
        );
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            capsfilter,
            sink,
            NULL
        );
        return;
    }

    GstPad *queue_sink_pad = gst_element_get_static_pad(queue, "sink");
    if (queue_sink_pad == NULL) {
        LOGE("Could not get H.264 WebRTC encoded queue sink pad");
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            capsfilter,
            sink,
            NULL
        );
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
    gst_object_unref(queue_sink_pad);

    if (ret != GST_PAD_LINK_OK) {
        LOGE(
            "Could not link incoming WebRTC pad to H.264 encoded queue. ret=%d",
            ret
        );
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            capsfilter,
            sink,
            NULL
        );
        return;
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(capsfilter);
    gst_element_sync_state_with_parent(sink);

    LOGI(
        "Incoming WebRTC media linked to H.264 ENCODED access-unit appsink pipeline."
    );
}

static void on_ice_candidate(GstElement *webrtc, guint mline_index, gchar *candidate, gpointer user_data)
{
    WebRTCSession *session = (WebRTCSession*)user_data;

    if (session == NULL || candidate == NULL || candidate[0] == '\0') {
        return;
    }

    gchar *encoded = g_base64_encode((const guchar*)candidate, strlen(candidate));

    if (encoded == NULL) {
        LOGE("Failed to base64 encode local ICE candidate");
        return;
    }

    int ok = send_line_format(session, "ICE|%u|%s", mline_index, encoded);

    if (ok) {
        LOGI("Sent local ICE candidate to Windows sender");
    } else {
        LOGE("Failed to send local ICE candidate to Windows sender. Signaling may already be closed.");
    }

    g_free(encoded);
}

static WebRTCSession *webrtc_session_new(int client_fd)
{
    WebRTCSession *session = (WebRTCSession*)g_malloc0(sizeof(WebRTCSession));

    if (session == NULL) {
        LOGE("Could not allocate WebRTCSession");
        return NULL;
    }

    session->client_fd = client_fd;
    session->pipeline = NULL;
    session->webrtc = NULL;
    pthread_mutex_init(&session->send_mutex, NULL);

    session->pipeline = gst_pipeline_new("quest-webrtc-receiver-pipeline");
    session->webrtc = gst_element_factory_make("webrtcbin", "webrtc");

    if (session->pipeline == NULL || session->webrtc == NULL) {
        LOGE("Could not create WebRTC receiver pipeline or webrtcbin");

        if (session->pipeline != NULL) {
            gst_object_unref(session->pipeline);
        }

        pthread_mutex_destroy(&session->send_mutex);
        g_free(session);

        return NULL;
    }

    gst_bin_add(GST_BIN(session->pipeline), session->webrtc);

    g_signal_connect(session->webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), session);
    g_signal_connect(session->webrtc, "pad-added", G_CALLBACK(on_incoming_stream), session);

    GstStateChangeReturn state_ret = gst_element_set_state(session->pipeline, GST_STATE_PLAYING);

    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set WebRTC receiver pipeline to PLAYING");

        gst_object_unref(session->pipeline);
        pthread_mutex_destroy(&session->send_mutex);
        g_free(session);

        return NULL;
    }

    LOGI("Quest WebRTC receiver pipeline created and set to PLAYING");

    return session;
}

static void webrtc_session_detach_signaling(WebRTCSession *session)
{
    if (session == NULL) {
        return;
    }

    pthread_mutex_lock(&session->send_mutex);
    session->client_fd = -1;
    pthread_mutex_unlock(&session->send_mutex);

    LOGI("Signaling TCP connection detached from WebRTC session; media pipeline remains alive");
}

static void webrtc_session_destroy(WebRTCSession *session)
{
    if (session == NULL) {
        return;
    }

    pthread_mutex_lock(&session->send_mutex);
    session->client_fd = -1;
    pthread_mutex_unlock(&session->send_mutex);

    if (session->pipeline != NULL) {
        LOGI("Stopping Quest WebRTC receiver pipeline");
        gst_element_set_state(session->pipeline, GST_STATE_NULL);
        gst_object_unref(session->pipeline);
        session->pipeline = NULL;
        session->webrtc = NULL;
    }

    pthread_mutex_destroy(&session->send_mutex);
    g_free(session);
}

static int handle_offer_message(WebRTCSession *session, const char *encoded_offer)
{
    if (session == NULL || session->webrtc == NULL || encoded_offer == NULL) {
        return 0;
    }

    char *sdp_text = base64_decode_to_text(encoded_offer);

    if (sdp_text == NULL) {
        LOGE("Could not base64 decode SDP offer");
        return 0;
    }

    gboolean offer_is_av1 =
        g_strstr_len(sdp_text, -1, "AV1/90000") != NULL ||
        g_strstr_len(sdp_text, -1, "av1/90000") != NULL;

    LOGI("Received SDP offer from Windows sender");
    set_last_message("Received WebRTC OFFER");

    GstSDPMessage *sdp = NULL;
    GstSDPResult sdp_result = gst_sdp_message_new(&sdp);

    if (sdp_result != GST_SDP_OK || sdp == NULL) {
        LOGE("Could not allocate SDP message");
        g_free(sdp_text);
        return 0;
    }

    sdp_result = gst_sdp_message_parse_buffer(
        (const guint8*)sdp_text,
        strlen(sdp_text),
        sdp
    );

    g_free(sdp_text);

    if (sdp_result != GST_SDP_OK) {
        LOGE("Could not parse SDP offer. result=%d", sdp_result);
        gst_sdp_message_free(sdp);
        return 0;
    }

    GstWebRTCSessionDescription *offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

    if (offer == NULL) {
        LOGE("Could not create WebRTC offer session description");
        gst_sdp_message_free(sdp);
        return 0;
    }

    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(session->webrtc, "set-remote-description", offer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);

    LOGI("Remote SDP offer applied");

    /* QGXS AV1 receiver NACK/RTX recovery */
    if (offer_is_av1) {
        GstWebRTCRTPTransceiver *transceiver = NULL;
        gboolean do_nack = FALSE;

        g_signal_emit_by_name(
            session->webrtc,
            "get-transceiver",
            0,
            &transceiver
        );

        if (transceiver == NULL) {
            LOGE(
                "AV1 WebRTC receiver transceiver 0 "
                "was not available"
            );
        } else {
            g_object_set(
                transceiver,
                "do-nack",
                TRUE,
                NULL
            );

            g_object_get(
                transceiver,
                "do-nack",
                &do_nack,
                NULL
            );

            LOGI(
                "AV1 WebRTC receiver loss recovery configured: "
                "do-nack=%d",
                do_nack ? 1 : 0
            );

            if (!do_nack) {
                LOGE(
                    "AV1 WebRTC receiver failed to enable do-nack"
                );
            }

            g_object_unref(transceiver);
        }
    }

    promise = gst_promise_new();
    g_signal_emit_by_name(session->webrtc, "create-answer", NULL, promise);
    gst_promise_wait(promise);

    const GstStructure *reply = gst_promise_get_reply(promise);

    if (reply == NULL) {
        LOGE("create-answer returned empty promise reply");
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(offer);
        return 0;
    }

    GstWebRTCSessionDescription *answer = NULL;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);

    gst_promise_unref(promise);

    if (answer == NULL) {
        LOGE("Could not extract WebRTC answer from promise reply");
        gst_webrtc_session_description_free(offer);
        return 0;
    }

    promise = gst_promise_new();
    g_signal_emit_by_name(session->webrtc, "set-local-description", answer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);

    LOGI("Local SDP answer applied");

    gchar *answer_sdp_text = gst_sdp_message_as_text(answer->sdp);

    if (answer_sdp_text == NULL) {
        LOGE("Could not convert SDP answer to text");
        gst_webrtc_session_description_free(answer);
        gst_webrtc_session_description_free(offer);
        return 0;
    }

    gchar *encoded_answer = g_base64_encode(
        (const guchar*)answer_sdp_text,
        strlen(answer_sdp_text)
    );

    g_free(answer_sdp_text);

    if (encoded_answer == NULL) {
        LOGE("Could not base64 encode SDP answer");
        gst_webrtc_session_description_free(answer);
        gst_webrtc_session_description_free(offer);
        return 0;
    }

    int ok = send_line_format(session, "ANSWER|%s", encoded_answer);

    if (ok) {
        LOGI("Sent SDP answer to Windows sender");
        set_last_message("Sent WebRTC ANSWER");
    } else {
        LOGE("Failed to send SDP answer to Windows sender");
    }

    g_free(encoded_answer);

    gst_webrtc_session_description_free(answer);
    gst_webrtc_session_description_free(offer);

    return ok;
}

static int handle_ice_message(WebRTCSession *session, char *line)
{
    if (session == NULL || session->webrtc == NULL || line == NULL) {
        return 0;
    }

    char *first = strchr(line, '|');

    if (first == NULL) {
        LOGE("Invalid ICE line: missing first separator");
        return 0;
    }

    char *second = strchr(first + 1, '|');

    if (second == NULL) {
        LOGE("Invalid ICE line: missing second separator");
        return 0;
    }

    *second = '\0';

    const char *mline_text = first + 1;
    const char *encoded_candidate = second + 1;

    char *candidate = base64_decode_to_text(encoded_candidate);

    if (candidate == NULL) {
        LOGE("Could not base64 decode remote ICE candidate");
        return 0;
    }

    char *endptr = NULL;
    unsigned long mline_index_ul = strtoul(mline_text, &endptr, 10);

    if (endptr == mline_text) {
        LOGE("Invalid ICE mline index: %s", mline_text);
        g_free(candidate);
        return 0;
    }

    guint mline_index = (guint)mline_index_ul;

    g_signal_emit_by_name(session->webrtc, "add-ice-candidate", mline_index, candidate);

    LOGI("Remote ICE candidate applied. mline=%u", mline_index);
    set_last_message("Received remote ICE candidate");

    g_free(candidate);

    return 1;
}

static void* signaling_thread_main(void *arg)
{
    int port = *((int*)arg);
    free(arg);

    int server_fd = -1;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        LOGE("Signaling socket() failed: errno=%d", errno);
        g_signaling_running = 0;
        return NULL;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Signaling bind() failed on port %d: errno=%d", port, errno);
        close(server_fd);
        g_signaling_running = 0;
        return NULL;
    }

    if (listen(server_fd, 4) < 0) {
        LOGE("Signaling listen() failed: errno=%d", errno);
        close(server_fd);
        g_signaling_running = 0;
        return NULL;
    }

    g_signaling_server_fd = server_fd;
    g_signaling_running = 1;

    LOGI("Native WebRTC signaling server started on port %d", port);
    set_last_message("Native WebRTC signaling server started");

    while (g_signaling_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        LOGI("Waiting for signaling client...");

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (g_signaling_running) {
                LOGE("Signaling accept() failed: errno=%d", errno);
            }
            break;
        }

        char client_ip[64] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        LOGI("Signaling client connected from %s", client_ip);
        set_last_message("Signaling client connected");

        WebRTCSession *session = NULL;

        char *line = (char*)malloc(MAX_SIGNALING_LINE);

        if (line == NULL) {
            LOGE("Could not allocate signaling line buffer");
            close(client_fd);
            continue;
        }

        while (g_signaling_running) {
            int received = recv_line_alloc(client_fd, line, MAX_SIGNALING_LINE);

            if (received <= 0) {
                LOGI("Signaling client disconnected");
                break;
            }

            if (strcmp(line, "hello from windows") == 0) {
                const char *reply = "hello from quest native signaling";
                send_all(client_fd, reply, strlen(reply));
                send_all(client_fd, "\n", 1);
                LOGI("Handled legacy hello message");
                set_last_message("hello from windows");
                continue;
            }

            if (strncmp(line, "OFFER|", 6) == 0) {
                if (session == NULL) {
                    session = webrtc_session_new(client_fd);
                }

                if (session == NULL) {
                    LOGE("Cannot handle OFFER because WebRTC session could not be created");
                    continue;
                }

                handle_offer_message(session, line + 6);
                poll_pipeline_bus(session);
                continue;
            }

            if (strncmp(line, "ICE|", 4) == 0) {
                if (session == NULL) {
                    LOGI("Received ICE before OFFER; creating WebRTC session");
                    session = webrtc_session_new(client_fd);
                }

                if (session == NULL) {
                    LOGE("Cannot handle ICE because WebRTC session could not be created");
                    continue;
                }

                handle_ice_message(session, line);
                poll_pipeline_bus(session);
                continue;
            }

            LOGI("Unknown signaling line: %.120s", line);
        }

        if (session != NULL) {
            webrtc_session_detach_signaling(session);

            if (client_fd >= 0) {
                close(client_fd);
                client_fd = -1;
            }

            LOGI("Signaling TCP connection closed; destroying the old WebRTC session and returning to accept().");
            set_last_message("Signaling closed; ready for a new WebRTC session");

            poll_pipeline_bus(session);
            webrtc_session_destroy(session);
            session = NULL;

            LOGI("Old WebRTC session destroyed. Receiver is ready for sender reconnection.");
        }

        free(line);

        if (client_fd >= 0) {
            close(client_fd);
            client_fd = -1;
        }
    }

    if (server_fd >= 0) {
        close(server_fd);
    }

    g_signaling_server_fd = -1;
    g_signaling_running = 0;

    LOGI("Native WebRTC signaling server stopped");

    return NULL;
}

JNIEXPORT int JNICALL gst_quest_start_signaling_test(int port)
{
    if (!g_gst_initialized) {
        LOGI("GStreamer was not initialized yet. Initializing now before starting signaling server.");

        if (!gst_quest_init()) {
            LOGE("Cannot start signaling server because GStreamer initialization failed.");
            return 0;
        }
    }

    if (g_signaling_running || g_signaling_thread_created) {
        LOGI("Native signaling server is already running or starting");
        return 1;
    }

    if (port <= 0 || port > 65535) {
        LOGE("Invalid signaling port: %d", port);
        return 0;
    }

    int *thread_port = (int*)malloc(sizeof(int));

    if (thread_port == NULL) {
        LOGE("Failed to allocate signaling thread port");
        return 0;
    }

    *thread_port = port;
    g_signaling_port = port;

    int result = pthread_create(&g_signaling_thread, NULL, signaling_thread_main, thread_port);

    if (result != 0) {
        LOGE("pthread_create failed: %d", result);
        free(thread_port);
        return 0;
    }

    g_signaling_thread_created = 1;

    LOGI("Native signaling thread created for port %d", port);

    return 1;
}

JNIEXPORT int JNICALL gst_quest_is_signaling_running()
{
    return g_signaling_running ? 1 : 0;
}

JNIEXPORT int JNICALL gst_quest_get_last_signaling_message(char *dst, int dst_size)
{
    if (dst == NULL || dst_size <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_message_mutex);
    snprintf(dst, (size_t)dst_size, "%s", g_last_message);
    pthread_mutex_unlock(&g_message_mutex);

    return 1;
}

JNIEXPORT void JNICALL gst_quest_stop_signaling_test()
{
    if (!g_signaling_thread_created && !g_signaling_running) {
        LOGI("Native signaling server is not running");
        return;
    }

    LOGI("Stopping native signaling server...");

    g_signaling_running = 0;

    if (g_signaling_server_fd >= 0) {
        shutdown(g_signaling_server_fd, SHUT_RDWR);
        close(g_signaling_server_fd);
        g_signaling_server_fd = -1;
    }

    if (g_signaling_thread_created) {
        pthread_join(g_signaling_thread, NULL);
        g_signaling_thread_created = 0;
    }

    set_last_message("Native signaling server stopped");

    LOGI("Native signaling server stopped cleanly");
}

static void* discovery_thread_main(void *arg)
{
    int port = *((int*)arg);
    free(arg);

    int sock_fd = -1;
    int opt = 1;

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock_fd < 0) {
        LOGE("Discovery socket() failed: errno=%d", errno);
        g_discovery_running = 0;
        return NULL;
    }

    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Discovery bind() failed on UDP port %d: errno=%d", port, errno);
        close(sock_fd);
        g_discovery_running = 0;
        return NULL;
    }

    g_discovery_socket_fd = sock_fd;
    g_discovery_running = 1;

    char started_msg[256];
    snprintf(started_msg, sizeof(started_msg), "UDP discovery server started on port %d", port);

    LOGI("%s", started_msg);
    set_last_discovery_message(started_msg);

    while (g_discovery_running) {
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        ssize_t received = recvfrom(
            sock_fd,
            buffer,
            sizeof(buffer) - 1,
            0,
            (struct sockaddr*)&client_addr,
            &client_len
        );

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            if (g_discovery_running) {
                LOGE("Discovery recvfrom() failed: errno=%d", errno);
            }

            continue;
        }

        buffer[received] = '\0';

        char client_ip[64] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        LOGI("Discovery packet from %s: %s", client_ip, buffer);

        if (strstr(buffer, DISCOVERY_REQUEST) != NULL) {
            char reply[256];

            snprintf(
                reply,
                sizeof(reply),
                "%s|%d\n",
                DISCOVERY_REPLY_PREFIX,
                g_signaling_port
            );

            sendto(
                sock_fd,
                reply,
                strlen(reply),
                0,
                (struct sockaddr*)&client_addr,
                client_len
            );

            char status_msg[512];
            snprintf(
                status_msg,
                sizeof(status_msg),
                "Discovery reply sent to %s: %s",
                client_ip,
                reply
            );

            LOGI("%s", status_msg);
            set_last_discovery_message(status_msg);
        }
    }

    if (sock_fd >= 0) {
        close(sock_fd);
    }

    g_discovery_socket_fd = -1;
    g_discovery_running = 0;

    LOGI("UDP discovery server stopped");

    return NULL;
}

JNIEXPORT int JNICALL gst_quest_start_discovery_test(int discovery_port)
{
    if (g_discovery_running || g_discovery_thread_created) {
        LOGI("UDP discovery server is already running or starting");
        return 1;
    }

    if (discovery_port <= 0 || discovery_port > 65535) {
        LOGE("Invalid discovery port: %d", discovery_port);
        return 0;
    }

    int *thread_port = (int*)malloc(sizeof(int));

    if (thread_port == NULL) {
        LOGE("Failed to allocate discovery thread port");
        return 0;
    }

    *thread_port = discovery_port;
    g_discovery_port = discovery_port;

    int result = pthread_create(&g_discovery_thread, NULL, discovery_thread_main, thread_port);

    if (result != 0) {
        LOGE("Discovery pthread_create failed: %d", result);
        free(thread_port);
        return 0;
    }

    g_discovery_thread_created = 1;

    LOGI("UDP discovery thread created for port %d", discovery_port);

    return 1;
}

JNIEXPORT int JNICALL gst_quest_is_discovery_running()
{
    return g_discovery_running ? 1 : 0;
}

JNIEXPORT int JNICALL gst_quest_get_last_discovery_message(char *dst, int dst_size)
{
    if (dst == NULL || dst_size <= 0) {
        return 0;
    }

    pthread_mutex_lock(&g_message_mutex);
    snprintf(dst, (size_t)dst_size, "%s", g_last_discovery_message);
    pthread_mutex_unlock(&g_message_mutex);

    return 1;
}

JNIEXPORT void JNICALL gst_quest_stop_discovery_test()
{
    if (!g_discovery_thread_created && !g_discovery_running) {
        LOGI("UDP discovery server is not running");
        return;
    }

    LOGI("Stopping UDP discovery server...");

    g_discovery_running = 0;

    if (g_discovery_socket_fd >= 0) {
        close(g_discovery_socket_fd);
        g_discovery_socket_fd = -1;
    }

    if (g_discovery_thread_created) {
        pthread_join(g_discovery_thread, NULL);
        g_discovery_thread_created = 0;
    }

    set_last_discovery_message("UDP discovery server stopped");

    LOGI("UDP discovery server stopped cleanly");
}
/* ============================================================
 * QUEST RTP/H.264 UDP RECEIVER EXPERIMENT
 *
 * First Quest streaming test:
 *   udpsrc :5004
 *   -> rtpjitterbuffer
 *   -> rtph264depay
 *   -> h264parse
 *   -> Android/Qualcomm H.264 decoder fallback chain
 *   -> videoconvert
 *   -> RGBA appsink
 *   -> existing store_latest_frame()
 *
 * This is NOT the final zero-copy path.
 * It intentionally reuses the existing RGBA CPU-copy path so Unity can
 * visually confirm that Quest receives and decodes RTP/H.264.
 * ============================================================ */

static pthread_t g_rtp_h264_thread;
static int g_rtp_h264_thread_created = 0;
static volatile int g_rtp_h264_running = 0;
static pthread_mutex_t g_rtp_h264_mutex = PTHREAD_MUTEX_INITIALIZER;
static GstElement *g_rtp_h264_pipeline = NULL;


/* ================= RTP/H264 DEBUG PAD PROBES ================= */

static volatile guint64 g_dbg_udp_src_count = 0;
static volatile guint64 g_dbg_depay_src_count = 0;
static volatile guint64 g_dbg_parse_src_count = 0;
static volatile guint64 g_dbg_decoder_src_count = 0;
static volatile guint64 g_dbg_convert_src_count = 0;
static volatile guint64 g_dbg_appsink_sink_count = 0;

static void reset_rtp_h264_debug_counters()
{
    g_dbg_udp_src_count = 0;
    g_dbg_depay_src_count = 0;
    g_dbg_parse_src_count = 0;
    g_dbg_decoder_src_count = 0;
    g_dbg_convert_src_count = 0;
    g_dbg_appsink_sink_count = 0;
}

static GstPadProbeReturn rtp_h264_debug_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    const gchar *label = (const gchar*)user_data;
    guint64 count = 0;

    if (g_strcmp0(label, "udp_src") == 0) {
        count = ++g_dbg_udp_src_count;
    } else if (g_strcmp0(label, "depay_src") == 0) {
        count = ++g_dbg_depay_src_count;
    } else if (g_strcmp0(label, "parse_src") == 0) {
        count = ++g_dbg_parse_src_count;
    } else if (g_strcmp0(label, "decoder_src") == 0) {
        count = ++g_dbg_decoder_src_count;
    } else if (g_strcmp0(label, "convert_src") == 0) {
        count = ++g_dbg_convert_src_count;
    } else if (g_strcmp0(label, "appsink_sink") == 0) {
        count = ++g_dbg_appsink_sink_count;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gsize size = buffer != NULL ? gst_buffer_get_size(buffer) : 0;

    if (count <= 10 || (count % 60) == 0) {
        LOGI(
            "RTP DEBUG PAD %s count=%llu size=%zu | udp=%llu depay=%llu parse=%llu decoder=%llu convert=%llu appsink=%llu",
            label,
            (unsigned long long)count,
            size,
            (unsigned long long)g_dbg_udp_src_count,
            (unsigned long long)g_dbg_depay_src_count,
            (unsigned long long)g_dbg_parse_src_count,
            (unsigned long long)g_dbg_decoder_src_count,
            (unsigned long long)g_dbg_convert_src_count,
            (unsigned long long)g_dbg_appsink_sink_count
        );
    }

    return GST_PAD_PROBE_OK;
}

static void attach_rtp_h264_debug_probe(GstElement *pipeline, const gchar *element_name, const gchar *pad_name, const gchar *label)
{
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), element_name);

    if (element == NULL) {
        LOGE("RTP DEBUG could not find element for probe: %s", element_name);
        return;
    }

    GstPad *pad = gst_element_get_static_pad(element, pad_name);

    if (pad == NULL) {
        LOGE("RTP DEBUG could not find pad %s on element %s", pad_name, element_name);
        gst_object_unref(element);
        return;
    }

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, rtp_h264_debug_probe, (gpointer)label, NULL);

    LOGI("RTP DEBUG attached probe: element=%s pad=%s label=%s", element_name, pad_name, label);

    gst_object_unref(pad);
    gst_object_unref(element);
}

/* ============================================================= */

static void* rtp_h264_thread_main(void *arg)
{
    int port = *((int*)arg);
    free(arg);

    g_rtp_h264_running = 1;

    LOGI("RTP/H.264 UDP receiver thread starting on port %d", port);
    set_last_message("RTP/H.264 UDP receiver starting");

    const gchar *selected_decoder_factory = NULL;
    GstElement *test_decoder = create_h264_decoder_with_fallback(&selected_decoder_factory);

    if (test_decoder == NULL || selected_decoder_factory == NULL) {
        LOGE("RTP/H.264 receiver cannot start because no H.264 decoder is available");
        if (test_decoder != NULL) {
            gst_object_unref(test_decoder);
        }
        g_rtp_h264_running = 0;
        set_last_message("RTP/H.264 receiver failed: no H.264 decoder");
        return NULL;
    }

    gst_object_unref(test_decoder);

    LOGI("RTP/H.264 receiver selected decoder: %s", selected_decoder_factory);

    gchar *pipeline_desc = g_strdup_printf(
        "udpsrc name=rtp_udp_src port=%d "
        "caps=\"application/x-rtp,media=(string)video,encoding-name=(string)H264,payload=(int)96,clock-rate=(int)90000\" "
        "! rtpjitterbuffer latency=50 drop-on-latency=true "
        "! rtph264depay name=rtp_h264_depay "
        "! h264parse name=rtp_h264_parse config-interval=-1 "
        "! %s name=rtp_h264_decoder "
        "! videoconvert name=rtp_h264_convert "
        "! video/x-raw,format=RGBA "
        "! appsink name=rtp_h264_rgba_sink emit-signals=true sync=false max-buffers=2 drop=true",
        port,
        selected_decoder_factory
    );

    LOGI("RTP/H.264 receiver pipeline: %s", pipeline_desc);

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &error);
    g_free(pipeline_desc);

    if (pipeline == NULL) {
        if (error != NULL) {
            LOGE("Could not create RTP/H.264 receiver pipeline: %s", error->message);
            g_error_free(error);
        } else {
            LOGE("Could not create RTP/H.264 receiver pipeline: unknown error");
        }

        g_rtp_h264_running = 0;
        set_last_message("RTP/H.264 receiver failed: pipeline creation failed");
        return NULL;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "rtp_h264_rgba_sink");

    if (sink == NULL) {
        LOGE("Could not find RTP/H.264 appsink element");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_rtp_h264_running = 0;
        set_last_message("RTP/H.264 receiver failed: appsink missing");
        return NULL;
    }

    g_signal_connect(sink, "new-sample", G_CALLBACK(on_decoded_sample), NULL);

    reset_rtp_h264_debug_counters();

    attach_rtp_h264_debug_probe(pipeline, "rtp_udp_src", "src", "udp_src");
    attach_rtp_h264_debug_probe(pipeline, "rtp_h264_depay", "src", "depay_src");
    attach_rtp_h264_debug_probe(pipeline, "rtp_h264_parse", "src", "parse_src");
    attach_rtp_h264_debug_probe(pipeline, "rtp_h264_decoder", "src", "decoder_src");
    attach_rtp_h264_debug_probe(pipeline, "rtp_h264_convert", "src", "convert_src");
    attach_rtp_h264_debug_probe(pipeline, "rtp_h264_rgba_sink", "sink", "appsink_sink");

    gst_object_unref(sink);

    pthread_mutex_lock(&g_rtp_h264_mutex);
    g_rtp_h264_pipeline = pipeline;
    pthread_mutex_unlock(&g_rtp_h264_mutex);

    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);

    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("Failed to set RTP/H.264 receiver pipeline to PLAYING");
        pthread_mutex_lock(&g_rtp_h264_mutex);
        g_rtp_h264_pipeline = NULL;
        pthread_mutex_unlock(&g_rtp_h264_mutex);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_rtp_h264_running = 0;
        set_last_message("RTP/H.264 receiver failed: PLAYING failed");
        return NULL;
    }

    LOGI("RTP/H.264 UDP receiver is PLAYING on port %d", port);
    set_last_message("RTP/H.264 UDP receiver playing");

    GstBus *bus = gst_element_get_bus(pipeline);

    while (g_rtp_h264_running) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED
        );

        if (msg == NULL) {
            continue;
        }

        const gchar *src_name = GST_MESSAGE_SRC_NAME(msg);

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
            {
                GError *err = NULL;
                gchar *debug = NULL;
                gst_message_parse_error(msg, &err, &debug);

                LOGE(
                    "RTP/H.264 receiver ERROR from %s: %s",
                    src_name != NULL ? src_name : "unknown",
                    err != NULL ? err->message : "unknown"
                );

                if (debug != NULL) {
                    LOGE("RTP/H.264 receiver DEBUG: %s", debug);
                }

                if (err != NULL) {
                    g_error_free(err);
                }

                if (debug != NULL) {
                    g_free(debug);
                }

                set_last_message("RTP/H.264 receiver error");
                g_rtp_h264_running = 0;
                break;
            }

            case GST_MESSAGE_WARNING:
            {
                GError *err = NULL;
                gchar *debug = NULL;
                gst_message_parse_warning(msg, &err, &debug);

                LOGI(
                    "RTP/H.264 receiver WARNING from %s: %s",
                    src_name != NULL ? src_name : "unknown",
                    err != NULL ? err->message : "unknown"
                );

                if (debug != NULL) {
                    LOGI("RTP/H.264 receiver WARNING DEBUG: %s", debug);
                }

                if (err != NULL) {
                    g_error_free(err);
                }

                if (debug != NULL) {
                    g_free(debug);
                }

                break;
            }

            case GST_MESSAGE_EOS:
                LOGI("RTP/H.264 receiver EOS");
                g_rtp_h264_running = 0;
                break;

            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                    GstState old_state;
                    GstState new_state;
                    GstState pending_state;

                    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

                    LOGI(
                        "RTP/H.264 receiver state changed: %s -> %s",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state)
                    );
                }
                break;

            default:
                break;
        }

        gst_message_unref(msg);
    }

    if (bus != NULL) {
        gst_object_unref(bus);
    }

    LOGI("Stopping RTP/H.264 receiver pipeline");
    gst_element_set_state(pipeline, GST_STATE_NULL);

    pthread_mutex_lock(&g_rtp_h264_mutex);
    if (g_rtp_h264_pipeline == pipeline) {
        g_rtp_h264_pipeline = NULL;
    }
    pthread_mutex_unlock(&g_rtp_h264_mutex);

    gst_object_unref(pipeline);

    g_rtp_h264_running = 0;
    set_last_message("RTP/H.264 UDP receiver stopped");

    LOGI("RTP/H.264 receiver thread stopped");

    return NULL;
}

JNIEXPORT int JNICALL gst_quest_start_rtp_h264_receiver(int port)
{
    if (!g_gst_initialized) {
        LOGI("GStreamer was not initialized yet. Initializing before RTP/H.264 receiver.");

        if (!gst_quest_init()) {
            LOGE("Cannot start RTP/H.264 receiver because GStreamer initialization failed.");
            return 0;
        }
    }

    if (g_rtp_h264_running || g_rtp_h264_thread_created) {
        LOGI("RTP/H.264 receiver is already running or starting");
        return 1;
    }

    if (port <= 0 || port > 65535) {
        LOGE("Invalid RTP/H.264 receiver port: %d", port);
        return 0;
    }

    int *thread_port = (int*)malloc(sizeof(int));

    if (thread_port == NULL) {
        LOGE("Failed to allocate RTP/H.264 receiver thread port");
        return 0;
    }

    *thread_port = port;

    int result = pthread_create(&g_rtp_h264_thread, NULL, rtp_h264_thread_main, thread_port);

    if (result != 0) {
        LOGE("pthread_create failed for RTP/H.264 receiver: %d", result);
        free(thread_port);
        return 0;
    }

    g_rtp_h264_thread_created = 1;

    LOGI("RTP/H.264 receiver thread created for port %d", port);

    return 1;
}

JNIEXPORT int JNICALL gst_quest_is_rtp_h264_receiver_running()
{
    return g_rtp_h264_running ? 1 : 0;
}

JNIEXPORT void JNICALL gst_quest_stop_rtp_h264_receiver()
{
    if (!g_rtp_h264_thread_created && !g_rtp_h264_running) {
        LOGI("RTP/H.264 receiver is not running");
        return;
    }

    LOGI("Stopping RTP/H.264 receiver...");

    g_rtp_h264_running = 0;

    pthread_mutex_lock(&g_rtp_h264_mutex);
    GstElement *pipeline = g_rtp_h264_pipeline;
    if (pipeline != NULL) {
        gst_object_ref(pipeline);
    }
    pthread_mutex_unlock(&g_rtp_h264_mutex);

    if (pipeline != NULL) {
        gst_element_send_event(pipeline, gst_event_new_eos());
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    if (g_rtp_h264_thread_created) {
        pthread_join(g_rtp_h264_thread, NULL);
        g_rtp_h264_thread_created = 0;
    }

    set_last_message("RTP/H.264 receiver stopped");
    LOGI("RTP/H.264 receiver stopped cleanly");
}



/* ================= QUEST_HW_TEXTURE_SMOKE_TEST ================= */

#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdlib.h>
#include <android/log.h>

#define HW_TEX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GstQuestInit", __VA_ARGS__)
#define HW_TEX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GstQuestInit", __VA_ARGS__)

static GLuint g_hwtex_texture = 0;
static int g_hwtex_width = 512;
static int g_hwtex_height = 512;
static int g_hwtex_frame = 0;
static uint32_t *g_hwtex_pixels = NULL;
static int g_hwtex_pixels_count = 0;

typedef void (*UnityRenderingEvent)(int eventId);

static void hwtex_create_or_resize_locked()
{
    if (g_hwtex_width <= 0) g_hwtex_width = 512;
    if (g_hwtex_height <= 0) g_hwtex_height = 512;

    int needed = g_hwtex_width * g_hwtex_height;

    if (g_hwtex_pixels_count != needed) {
        if (g_hwtex_pixels != NULL) {
            free(g_hwtex_pixels);
            g_hwtex_pixels = NULL;
        }

        g_hwtex_pixels = (uint32_t*)malloc((size_t)needed * sizeof(uint32_t));
        g_hwtex_pixels_count = needed;

        HW_TEX_LOGI("HWTextureSmoke allocated CPU pattern buffer %dx%d", g_hwtex_width, g_hwtex_height);
    }

    if (g_hwtex_texture == 0) {
        glGenTextures(1, &g_hwtex_texture);
        glBindTexture(GL_TEXTURE_2D, g_hwtex_texture);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_hwtex_width, g_hwtex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        GLenum err = glGetError();
        HW_TEX_LOGI("HWTextureSmoke created GL_TEXTURE_2D id=%u size=%dx%d glError=0x%x",
                    g_hwtex_texture, g_hwtex_width, g_hwtex_height, err);

        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

static void hwtex_update_pattern_locked()
{
    hwtex_create_or_resize_locked();

    if (g_hwtex_texture == 0 || g_hwtex_pixels == NULL) {
        HW_TEX_LOGE("HWTextureSmoke update skipped because texture or pixels are missing");
        return;
    }

    int w = g_hwtex_width;
    int h = g_hwtex_height;
    int f = g_hwtex_frame;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t r = (uint8_t)((x + f * 5) & 255);
            uint8_t g = (uint8_t)((y + f * 3) & 255);
            uint8_t b = (uint8_t)(((x ^ y) + f * 7) & 255);
            uint8_t a = 255;

            int bar = ((x + f * 12) / 64) & 1;
            if (bar) {
                r = 255;
                g = (uint8_t)((y * 255) / h);
                b = 32;
            }

            g_hwtex_pixels[y * w + x] =
                ((uint32_t)a << 24) |
                ((uint32_t)b << 16) |
                ((uint32_t)g << 8) |
                ((uint32_t)r);
        }
    }

    glBindTexture(GL_TEXTURE_2D, g_hwtex_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_hwtex_pixels);
    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);

    g_hwtex_frame++;

    if (g_hwtex_frame <= 10 || (g_hwtex_frame % 60) == 0) {
        HW_TEX_LOGI("HWTextureSmoke updated texture id=%u frame=%d size=%dx%d glError=0x%x",
                    g_hwtex_texture, g_hwtex_frame, w, h, err);
    }
}

static void hwtex_on_render_event(int eventId)
{
    if (eventId == 1001) {
        hwtex_update_pattern_locked();
    }
}

__attribute__((visibility("default")))
void gst_quest_hwtex_configure(int width, int height)
{
    g_hwtex_width = width;
    g_hwtex_height = height;
    HW_TEX_LOGI("HWTextureSmoke configured size=%dx%d", g_hwtex_width, g_hwtex_height);
}

__attribute__((visibility("default")))
uintptr_t gst_quest_hwtex_get_texture_id()
{
    return (uintptr_t)g_hwtex_texture;
}

__attribute__((visibility("default")))
int gst_quest_hwtex_get_width()
{
    return g_hwtex_width;
}

__attribute__((visibility("default")))
int gst_quest_hwtex_get_height()
{
    return g_hwtex_height;
}

__attribute__((visibility("default")))
int gst_quest_hwtex_get_frame_id()
{
    return g_hwtex_frame;
}

__attribute__((visibility("default")))
UnityRenderingEvent gst_quest_hwtex_get_render_event_func()
{
    return hwtex_on_render_event;
}

/* =============================================================== */


/* ================= QUEST_GPU_FBO_TEXTURE_SMOKE_TEST ================= */

#include <GLES3/gl3.h>
#include <stdint.h>
#include <android/log.h>

#define GPU_TEX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GstQuestInit", __VA_ARGS__)
#define GPU_TEX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GstQuestInit", __VA_ARGS__)

typedef void (*QuestGpuUnityRenderingEvent)(int eventId);

static GLuint g_gputex_texture = 0;
static GLuint g_gputex_fbo = 0;
static GLuint g_gputex_program = 0;
static GLint g_gputex_u_frame = -1;
static int g_gputex_width = 1024;
static int g_gputex_height = 1024;
static int g_gputex_frame = 0;

static GLuint gputex_compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char logbuf[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(logbuf), &len, logbuf);
        GPU_TEX_LOGE("GPUTextureSmoke shader compile failed: %s", logbuf);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint gputex_create_program()
{
    const char *vs =
        "#version 300 es\n"
        "precision highp float;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    vec2 p;\n"
        "    if (gl_VertexID == 0) p = vec2(-1.0, -1.0);\n"
        "    else if (gl_VertexID == 1) p = vec2(3.0, -1.0);\n"
        "    else p = vec2(-1.0, 3.0);\n"
        "    v_uv = 0.5 * (p + 1.0);\n"
        "    gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";

    const char *fs =
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec2 v_uv;\n"
        "uniform float u_frame;\n"
        "out vec4 outColor;\n"
        "void main() {\n"
        "    float t = u_frame * 0.015;\n"
        "    float stripes = step(0.5, fract(v_uv.x * 8.0 + t));\n"
        "    vec3 a = vec3(v_uv.x, v_uv.y, 0.5 + 0.5 * sin(t));\n"
        "    vec3 b = vec3(1.0, 0.75 + 0.25 * sin(t + v_uv.y * 6.28318), 0.05);\n"
        "    vec3 c = mix(a, b, stripes);\n"
        "    c += 0.15 * vec3(sin((v_uv.x + t) * 20.0), sin((v_uv.y + t) * 17.0), sin((v_uv.x + v_uv.y + t) * 13.0));\n"
        "    outColor = vec4(c, 1.0);\n"
        "}\n";

    GLuint v = gputex_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = gputex_compile_shader(GL_FRAGMENT_SHADER, fs);

    if (v == 0 || f == 0) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);

    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);

    if (!ok) {
        char logbuf[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(p, sizeof(logbuf), &len, logbuf);
        GPU_TEX_LOGE("GPUTextureSmoke program link failed: %s", logbuf);
        glDeleteProgram(p);
        return 0;
    }

    g_gputex_u_frame = glGetUniformLocation(p, "u_frame");

    GPU_TEX_LOGI("GPUTextureSmoke shader program created id=%u u_frame=%d", p, g_gputex_u_frame);

    return p;
}

static void gputex_create_or_resize()
{
    if (g_gputex_width <= 0) g_gputex_width = 1024;
    if (g_gputex_height <= 0) g_gputex_height = 1024;

    if (g_gputex_texture == 0) {
        glGenTextures(1, &g_gputex_texture);
        glBindTexture(GL_TEXTURE_2D, g_gputex_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_gputex_width, g_gputex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &g_gputex_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_gputex_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gputex_texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        GLenum err = glGetError();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GPU_TEX_LOGI("GPUTextureSmoke created target texture id=%u fbo=%u size=%dx%d fboStatus=0x%x glError=0x%x",
                     g_gputex_texture, g_gputex_fbo, g_gputex_width, g_gputex_height, status, err);
    }

    if (g_gputex_program == 0) {
        g_gputex_program = gputex_create_program();
    }
}

static void gputex_render_frame()
{
    gputex_create_or_resize();

    if (g_gputex_texture == 0 || g_gputex_fbo == 0 || g_gputex_program == 0) {
        GPU_TEX_LOGE("GPUTextureSmoke render skipped because texture/fbo/program is missing");
        return;
    }

    GLint oldFbo = 0;
    GLint oldViewport[4] = {0, 0, 0, 0};
    GLint oldProgram = 0;

    GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);

    glBindFramebuffer(GL_FRAMEBUFFER, g_gputex_fbo);
    glViewport(0, 0, g_gputex_width, g_gputex_height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(g_gputex_program);
    if (g_gputex_u_frame >= 0) {
        glUniform1f(g_gputex_u_frame, (float)g_gputex_frame);
    }

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram((GLuint)oldProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldFbo);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    if (depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWasEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    GLenum err = glGetError();

    g_gputex_frame++;

    if (g_gputex_frame <= 10 || (g_gputex_frame % 60) == 0) {
        GPU_TEX_LOGI("GPUTextureSmoke rendered frame=%d texture=%u fbo=%u size=%dx%d glError=0x%x",
                     g_gputex_frame, g_gputex_texture, g_gputex_fbo, g_gputex_width, g_gputex_height, err);
    }
}

static void gputex_on_render_event(int eventId)
{
    if (eventId == 2001) {
        gputex_render_frame();
    }
}

__attribute__((visibility("default")))
void gst_quest_gputex_configure(int width, int height)
{
    g_gputex_width = width;
    g_gputex_height = height;
    GPU_TEX_LOGI("GPUTextureSmoke configured size=%dx%d", g_gputex_width, g_gputex_height);
}

__attribute__((visibility("default")))
uintptr_t gst_quest_gputex_get_texture_id()
{
    return (uintptr_t)g_gputex_texture;
}

__attribute__((visibility("default")))
int gst_quest_gputex_get_width()
{
    return g_gputex_width;
}

__attribute__((visibility("default")))
int gst_quest_gputex_get_height()
{
    return g_gputex_height;
}

__attribute__((visibility("default")))
int gst_quest_gputex_get_frame_id()
{
    return g_gputex_frame;
}

__attribute__((visibility("default")))
QuestGpuUnityRenderingEvent gst_quest_gputex_get_render_event_func()
{
    return gputex_on_render_event;
}

/* =============================================================== */


/* ================= QUEST_OES_SURFACE_TEXTURE_BRIDGE_TEST ================= */

#include <jni.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <android/log.h>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

#define OES_TEX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GstQuestInit", __VA_ARGS__)
#define OES_TEX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GstQuestInit", __VA_ARGS__)

typedef void (*QuestOesUnityRenderingEvent)(int eventId);

static GLuint g_oes_external_tex = 0;
static GLuint g_oes_target_tex = 0;
static GLuint g_oes_target_fbo = 0;
static GLuint g_oes_program = 0;
static int g_oes_width = 1024;
static int g_oes_height = 1024;
static int g_oes_frame = 0;

static jobject g_oes_surface_texture_obj = NULL;
static jmethodID g_oes_update_tex_image_mid = NULL;

static JNIEnv* oes_get_jni_env()
{
    if (g_quest_java_vm == NULL) {
        OES_TEX_LOGE("OESTextureBridge JavaVM is NULL. JNI_OnLoad did not store it.");
        return NULL;
    }

    JNIEnv *env = NULL;
    jint get_env_result = (*g_quest_java_vm)->GetEnv(g_quest_java_vm, (void**)&env, JNI_VERSION_1_6);

    if (get_env_result == JNI_EDETACHED) {
        if ((*g_quest_java_vm)->AttachCurrentThread(g_quest_java_vm, &env, NULL) != JNI_OK) {
            OES_TEX_LOGE("OESTextureBridge failed to attach current thread to JVM");
            return NULL;
        }
    } else if (get_env_result != JNI_OK) {
        OES_TEX_LOGE("OESTextureBridge GetEnv failed result=%d", get_env_result);
        return NULL;
    }

    return env;
}

static GLuint oes_compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char logbuf[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(logbuf), &len, logbuf);
        OES_TEX_LOGE("OESTextureBridge shader compile failed: %s", logbuf);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint oes_create_program()
{
    const char *vs =
        "#version 300 es\n"
        "precision highp float;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    vec2 p;\n"
        "    if (gl_VertexID == 0) p = vec2(-1.0, -1.0);\n"
        "    else if (gl_VertexID == 1) p = vec2(3.0, -1.0);\n"
        "    else p = vec2(-1.0, 3.0);\n"
        "    v_uv = 0.5 * (p + 1.0);\n"
        "    gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";

    const char *fs =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;\n"
        "in vec2 v_uv;\n"
        "uniform samplerExternalOES u_oes;\n"
        "out vec4 outColor;\n"
        "void main() {\n"
        "    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);\n"
        "    outColor = texture(u_oes, uv);\n"
        "}\n";

    GLuint v = oes_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = oes_compile_shader(GL_FRAGMENT_SHADER, fs);

    if (v == 0 || f == 0) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);

    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);

    if (!ok) {
        char logbuf[2048];
        GLsizei len = 0;
        glGetProgramInfoLog(p, sizeof(logbuf), &len, logbuf);
        OES_TEX_LOGE("OESTextureBridge program link failed: %s", logbuf);
        glDeleteProgram(p);
        return 0;
    }

    glUseProgram(p);
    GLint sampler = glGetUniformLocation(p, "u_oes");
    if (sampler >= 0) {
        glUniform1i(sampler, 0);
    }
    glUseProgram(0);

    OES_TEX_LOGI("OESTextureBridge shader program created id=%u sampler=%d", p, sampler);

    return p;
}

static void oes_create_or_resize()
{
    if (g_oes_width <= 0) g_oes_width = 1024;
    if (g_oes_height <= 0) g_oes_height = 1024;

    if (g_oes_external_tex == 0) {
        glGenTextures(1, &g_oes_external_tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_oes_external_tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

        GLenum err = glGetError();
        OES_TEX_LOGI("OESTextureBridge created external OES texture id=%u glError=0x%x",
                     g_oes_external_tex, err);
    }

    if (g_oes_target_tex == 0) {
        glGenTextures(1, &g_oes_target_tex);
        glBindTexture(GL_TEXTURE_2D, g_oes_target_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_oes_width, g_oes_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &g_oes_target_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_oes_target_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_oes_target_tex, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        GLenum err = glGetError();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        OES_TEX_LOGI("OESTextureBridge created target texture id=%u fbo=%u size=%dx%d fboStatus=0x%x glError=0x%x",
                     g_oes_target_tex, g_oes_target_fbo, g_oes_width, g_oes_height, status, err);
    }

    if (g_oes_program == 0) {
        g_oes_program = oes_create_program();
    }
}

static void oes_update_surface_texture()
{
    if (g_oes_surface_texture_obj == NULL || g_oes_update_tex_image_mid == NULL) {
        return;
    }

    JNIEnv *env = oes_get_jni_env();
    if (env == NULL) {
        return;
    }

    (*env)->CallVoidMethod(env, g_oes_surface_texture_obj, g_oes_update_tex_image_mid);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        OES_TEX_LOGE("OESTextureBridge updateTexImage threw Java exception");
    }
}

static void oes_blit_frame()
{
    oes_create_or_resize();

    if (g_oes_external_tex == 0 || g_oes_target_tex == 0 || g_oes_target_fbo == 0 || g_oes_program == 0) {
        OES_TEX_LOGE("OESTextureBridge blit skipped because texture/fbo/program is missing");
        return;
    }

    oes_update_surface_texture();

    GLint oldFbo = 0;
    GLint oldViewport[4] = {0, 0, 0, 0};
    GLint oldProgram = 0;
    GLint oldActiveTexture = 0;
    GLint oldTex2D = 0;
    GLint oldTexOes = 0;

    GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex2D);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &oldTexOes);

    glBindFramebuffer(GL_FRAMEBUFFER, g_oes_target_fbo);
    glViewport(0, 0, g_oes_width, g_oes_height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(g_oes_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_oes_external_tex);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)oldTexOes);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex2D);
    glActiveTexture((GLenum)oldActiveTexture);
    glUseProgram((GLuint)oldProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldFbo);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    if (depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWasEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    GLenum err = glGetError();

    g_oes_frame++;

    if (g_oes_frame <= 10 || (g_oes_frame % 60) == 0) {
        OES_TEX_LOGI("OESTextureBridge blitted frame=%d oesTex=%u targetTex=%u fbo=%u size=%dx%d glError=0x%x",
                     g_oes_frame, g_oes_external_tex, g_oes_target_tex, g_oes_target_fbo, g_oes_width, g_oes_height, err);
    }
}

static void oes_on_render_event(int eventId)
{
    if (eventId == 3001) {
        oes_blit_frame();
    } else if (eventId == 3000) {
        oes_create_or_resize();
    }
}

__attribute__((visibility("default")))
void gst_quest_oes_configure(int width, int height)
{
    g_oes_width = width;
    g_oes_height = height;
    OES_TEX_LOGI("OESTextureBridge configured size=%dx%d", g_oes_width, g_oes_height);
}

__attribute__((visibility("default")))
uintptr_t gst_quest_oes_get_external_texture_id()
{
    return (uintptr_t)g_oes_external_tex;
}

__attribute__((visibility("default")))
uintptr_t gst_quest_oes_get_target_texture_id()
{
    return (uintptr_t)g_oes_target_tex;
}

__attribute__((visibility("default")))
int gst_quest_oes_get_width()
{
    return g_oes_width;
}

__attribute__((visibility("default")))
int gst_quest_oes_get_height()
{
    return g_oes_height;
}

__attribute__((visibility("default")))
int gst_quest_oes_get_frame_id()
{
    return g_oes_frame;
}

__attribute__((visibility("default")))
void gst_quest_oes_set_surface_texture(void *surface_texture_obj)
{
    JNIEnv *env = oes_get_jni_env();
    if (env == NULL) {
        OES_TEX_LOGE("OESTextureBridge set_surface_texture failed: env is NULL");
        return;
    }

    if (g_oes_surface_texture_obj != NULL) {
        (*env)->DeleteGlobalRef(env, g_oes_surface_texture_obj);
        g_oes_surface_texture_obj = NULL;
        g_oes_update_tex_image_mid = NULL;
    }

    if (surface_texture_obj == NULL) {
        OES_TEX_LOGI("OESTextureBridge cleared SurfaceTexture reference");
        return;
    }

    jobject localObj = (jobject)surface_texture_obj;
    g_oes_surface_texture_obj = (*env)->NewGlobalRef(env, localObj);

    jclass cls = (*env)->GetObjectClass(env, g_oes_surface_texture_obj);
    g_oes_update_tex_image_mid = (*env)->GetMethodID(env, cls, "updateTexImage", "()V");

    if (cls != NULL) {
        (*env)->DeleteLocalRef(env, cls);
    }

    OES_TEX_LOGI("OESTextureBridge stored SurfaceTexture globalRef=%p updateTexImage=%p",
                 g_oes_surface_texture_obj, g_oes_update_tex_image_mid);
}

__attribute__((visibility("default")))
QuestOesUnityRenderingEvent gst_quest_oes_get_render_event_func()
{
    return oes_on_render_event;
}

/* =============================================================== */





/* ================= QUEST_NATIVE_H265_RTP_AMEDIACODEC_RECEIVER ================= */

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window_jni.h>

#define NH265_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GstQuestNativeH265", __VA_ARGS__)
#define NH265_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GstQuestNativeH265", __VA_ARGS__)

static pthread_t g_native_h265_thread;
static volatile int g_native_h265_running = 0;
static volatile int g_native_h265_stop = 0;

static jobject g_native_h265_surface_global = NULL;
static int g_native_h265_width = 4096;
static int g_native_h265_height = 2048;
static int g_native_h265_port = 5008;
static int g_native_h265_fps = 60;

static pthread_mutex_t g_native_h265_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_native_h265_rtp_packets = 0;
static unsigned long long g_native_h265_access_units = 0;
static unsigned long long g_native_h265_decoded_frames = 0;
static unsigned long long g_native_h265_dropped_packets = 0;

typedef struct NativeH265Buffer {
    uint8_t *data;
    size_t size;
    size_t cap;
} NativeH265Buffer;

static int nh265_reserve(NativeH265Buffer *b, size_t extra)
{
    if (!b) return 0;
    size_t need = b->size + extra;
    if (need <= b->cap) return 1;

    size_t new_cap = b->cap ? b->cap : 65536;
    while (new_cap < need) new_cap *= 2;

    uint8_t *p = (uint8_t*)realloc(b->data, new_cap);
    if (!p) return 0;

    b->data = p;
    b->cap = new_cap;
    return 1;
}

static int nh265_append(NativeH265Buffer *b, const uint8_t *src, size_t len)
{
    if (!b || !src || len == 0) return 0;
    if (!nh265_reserve(b, len)) return 0;
    memcpy(b->data + b->size, src, len);
    b->size += len;
    return 1;
}

static int nh265_append_start_code(NativeH265Buffer *b)
{
    static const uint8_t sc[4] = {0,0,0,1};
    return nh265_append(b, sc, 4);
}

static int nh265_append_nal(NativeH265Buffer *au, const uint8_t *nal, size_t nal_len)
{
    if (!au || !nal || nal_len == 0) return 0;
    if (!nh265_append_start_code(au)) return 0;
    return nh265_append(au, nal, nal_len);
}

static void nh265_clear(NativeH265Buffer *b)
{
    if (b) b->size = 0;
}

static void nh265_free(NativeH265Buffer *b)
{
    if (!b) return;
    if (b->data) free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

static void nh265_inc_rtp(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_rtp_packets++;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
}

static void nh265_inc_drop(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_dropped_packets++;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
}

static void nh265_inc_au(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_access_units++;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
}

static void nh265_inc_decoded(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_decoded_frames++;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
}

static JNIEnv* nh265_get_env(int *attached)
{
    if (attached) *attached = 0;

    if (g_quest_java_vm == NULL) {
        NH265_LOGE("JavaVM is NULL.");
        return NULL;
    }

    JNIEnv *env = NULL;
    jint r = (*g_quest_java_vm)->GetEnv(g_quest_java_vm, (void**)&env, JNI_VERSION_1_6);

    if (r == JNI_OK) return env;

    if (r == JNI_EDETACHED) {
        if ((*g_quest_java_vm)->AttachCurrentThread(g_quest_java_vm, &env, NULL) == JNI_OK) {
            if (attached) *attached = 1;
            return env;
        }
    }

    return NULL;
}

static void nh265_detach_env(int attached)
{
    if (attached && g_quest_java_vm != NULL) {
        (*g_quest_java_vm)->DetachCurrentThread(g_quest_java_vm);
    }
}

static int nh265_queue_access_unit(AMediaCodec *codec, NativeH265Buffer *au, int fps, int64_t *pts_us)
{
    if (!codec || !au || au->size == 0) return 0;

    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 1000);
    if (idx < 0) return 0;

    size_t input_size = 0;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)idx, &input_size);

    if (!input || input_size < au->size) {
        nh265_inc_drop();
        return 0;
    }

    memcpy(input, au->data, au->size);

    media_status_t st = AMediaCodec_queueInputBuffer(
        codec,
        (size_t)idx,
        0,
        au->size,
        *pts_us,
        0
    );

    if (st != AMEDIA_OK) {
        nh265_inc_drop();
        return 0;
    }

    int step = fps > 0 ? (1000000 / fps) : 16666;
    *pts_us += step;
    nh265_inc_au();
    return 1;
}

static void nh265_drain_codec(AMediaCodec *codec)
{
    if (!codec) return;

    AMediaCodecBufferInfo info;

    for (;;) {
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);

        if (out >= 0) {
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out, info.size > 0);
            if (info.size > 0) {
                nh265_inc_decoded();
            }
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
            if (fmt) {
                const char *s = AMediaFormat_toString(fmt);
                NH265_LOGI("Output format changed: %s", s ? s : "NULL");
                AMediaFormat_delete(fmt);
            }
        } else {
            break;
        }
    }
}


/* ================= WEBRTC_H265_SURFACE_AMEDIACODEC_START ================= */

static pthread_mutex_t g_webrtc_h265_codec_mutex = PTHREAD_MUTEX_INITIALIZER;
static AMediaCodec *g_webrtc_h265_codec = NULL;
static ANativeWindow *g_webrtc_h265_window = NULL;
static jobject g_webrtc_h265_surface_global = NULL;
static volatile int g_webrtc_h265_surface_running = 0;
static int g_webrtc_h265_width = 3072;
static int g_webrtc_h265_height = 1536;
static int g_webrtc_h265_fps = 120;
static int64_t g_webrtc_h265_pts_us = 0;
static unsigned long long g_webrtc_h265_input_aus = 0;
static unsigned long long g_webrtc_h265_drops = 0;

static void webrtc_h265_reset_native_stats(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_rtp_packets = 0;
    g_native_h265_access_units = 0;
    g_native_h265_decoded_frames = 0;
    g_native_h265_dropped_packets = 0;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
}

__attribute__((visibility("default")))
void gst_quest_webrtc_h265_stop_surface(void)
{
    AMediaCodec *codec = NULL;
    ANativeWindow *window = NULL;
    jobject surface_global = NULL;

    pthread_mutex_lock(&g_webrtc_h265_codec_mutex);

    codec = g_webrtc_h265_codec;
    window = g_webrtc_h265_window;
    surface_global = g_webrtc_h265_surface_global;

    g_webrtc_h265_codec = NULL;
    g_webrtc_h265_window = NULL;
    g_webrtc_h265_surface_global = NULL;
    g_webrtc_h265_surface_running = 0;
    g_webrtc_h265_pts_us = 0;

    pthread_mutex_unlock(&g_webrtc_h265_codec_mutex);

    if (codec != NULL) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }

    if (window != NULL) {
        ANativeWindow_release(window);
    }

    if (surface_global != NULL) {
        int attached = 0;
        JNIEnv *env = nh265_get_env(&attached);
        if (env != NULL) {
            (*env)->DeleteGlobalRef(env, surface_global);
        }
        nh265_detach_env(attached);
    }

    NH265_LOGI("WebRTC H265 Surface decoder stopped.");
}

__attribute__((visibility("default")))
int gst_quest_webrtc_h265_start_surface(void *surface_obj, int width, int height, int fps)
{
    if (surface_obj == NULL) {
        NH265_LOGE("WebRTC H265 start failed: surface is NULL.");
        return 0;
    }

    gst_quest_webrtc_h265_stop_surface();

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);

    if (env == NULL) {
        NH265_LOGE("WebRTC H265 start failed: JNI env unavailable.");
        return 0;
    }

    jobject surface_global = (*env)->NewGlobalRef(env, (jobject)surface_obj);

    if (surface_global == NULL) {
        NH265_LOGE("WebRTC H265 start failed: NewGlobalRef returned NULL.");
        nh265_detach_env(attached);
        return 0;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface_global);

    if (window == NULL) {
        NH265_LOGE("WebRTC H265 start failed: ANativeWindow_fromSurface failed.");
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/hevc");

    if (codec == NULL) {
        NH265_LOGE("WebRTC H265 start failed: AMediaCodec_createDecoderByType(video/hevc) failed.");
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, "frame-rate", fps > 0 ? fps : 60);
    AMediaFormat_setInt32(fmt, "max-input-size", 4 * 1024 * 1024);

    media_status_t st = AMediaCodec_configure(codec, fmt, window, NULL, 0);
    AMediaFormat_delete(fmt);

    if (st != AMEDIA_OK) {
        NH265_LOGE("WebRTC H265 AMediaCodec_configure failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    st = AMediaCodec_start(codec);

    if (st != AMEDIA_OK) {
        NH265_LOGE("WebRTC H265 AMediaCodec_start failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    pthread_mutex_lock(&g_webrtc_h265_codec_mutex);

    g_webrtc_h265_codec = codec;
    g_webrtc_h265_window = window;
    g_webrtc_h265_surface_global = surface_global;
    g_webrtc_h265_width = width;
    g_webrtc_h265_height = height;
    g_webrtc_h265_fps = fps > 0 ? fps : 60;
    g_webrtc_h265_pts_us = 0;
    g_webrtc_h265_input_aus = 0;
    g_webrtc_h265_drops = 0;
    g_webrtc_h265_surface_running = 1;

    pthread_mutex_unlock(&g_webrtc_h265_codec_mutex);

    webrtc_h265_reset_native_stats();

    nh265_detach_env(attached);

    NH265_LOGI(
        "WebRTC H265 Surface decoder started size=%dx%d fps=%d",
        width,
        height,
        fps > 0 ? fps : 60
    );

    return 1;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_h265_get_running(void)
{
    return g_webrtc_h265_surface_running ? 1 : 0;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h265_get_input_units(void)
{
    return g_webrtc_h265_input_aus;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h265_get_decoded_frames(void)
{
    unsigned long long value;
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    value = g_native_h265_decoded_frames;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
    return value;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h265_get_drops(void)
{
    return g_webrtc_h265_drops;
}

static int webrtc_h265_queue_encoded_gst_buffer(GstBuffer *buffer)
{
    if (buffer == NULL) return 0;

    GstMapInfo map;
    memset(&map, 0, sizeof(map));

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        NH265_LOGE("WebRTC H265 could not map GstBuffer.");
        return 0;
    }

    int queued = 0;

    pthread_mutex_lock(&g_webrtc_h265_codec_mutex);

    if (g_webrtc_h265_surface_running && g_webrtc_h265_codec != NULL && map.data != NULL && map.size > 0) {
        NativeH265Buffer au;
        au.data = (uint8_t*)map.data;
        au.size = map.size;
        au.cap = map.size;

        queued = nh265_queue_access_unit(
            g_webrtc_h265_codec,
            &au,
            g_webrtc_h265_fps,
            &g_webrtc_h265_pts_us
        );

        nh265_drain_codec(g_webrtc_h265_codec);

        if (queued) {
            g_webrtc_h265_input_aus++;
            if (g_webrtc_h265_input_aus == 1 || g_webrtc_h265_input_aus % 120 == 0) {
                NH265_LOGI(
                    "WebRTC H265 queued AU to AMediaCodec: au=%llu bytes=%lu size=%dx%d fps=%d",
                    g_webrtc_h265_input_aus,
                    (unsigned long)map.size,
                    g_webrtc_h265_width,
                    g_webrtc_h265_height,
                    g_webrtc_h265_fps
                );
            }
        } else {
            g_webrtc_h265_drops++;
            if (g_webrtc_h265_drops == 1 || g_webrtc_h265_drops % 60 == 0) {
                NH265_LOGE(
                    "WebRTC H265 failed to queue AU to AMediaCodec: drops=%llu bytes=%lu",
                    g_webrtc_h265_drops,
                    (unsigned long)map.size
                );
            }
        }
    } else {
        g_webrtc_h265_drops++;
        if (g_webrtc_h265_drops == 1 || g_webrtc_h265_drops % 120 == 0) {
            NH265_LOGE(
                "WebRTC H265 Surface decoder not ready; dropping AU drops=%llu bytes=%lu running=%d codec=%p",
                g_webrtc_h265_drops,
                (unsigned long)map.size,
                g_webrtc_h265_surface_running,
                g_webrtc_h265_codec
            );
        }
    }

    pthread_mutex_unlock(&g_webrtc_h265_codec_mutex);

    gst_buffer_unmap(buffer, &map);

    return queued;
}

/* ================= WEBRTC_H265_SURFACE_AMEDIACODEC_END ================= */

/* ================= WEBRTC_AV1_SURFACE_AMEDIACODEC_START ================= */

static pthread_mutex_t g_webrtc_av1_codec_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static AMediaCodec *g_webrtc_av1_codec = NULL;
static ANativeWindow *g_webrtc_av1_window = NULL;
static jobject g_webrtc_av1_surface_global = NULL;

static volatile int g_webrtc_av1_surface_running = 0;

static int g_webrtc_av1_width = 4096;
static int g_webrtc_av1_height = 2048;
static int g_webrtc_av1_fps = 120;

static int64_t g_webrtc_av1_pts_us = 0;

static unsigned long long g_webrtc_av1_input_units = 0;
static unsigned long long g_webrtc_av1_decoded_frames = 0;
static unsigned long long g_webrtc_av1_drops = 0;

static void webrtc_av1_drain_codec_locked(void)
{
    if (g_webrtc_av1_codec == NULL) {
        return;
    }

    AMediaCodecBufferInfo info;
    memset(&info, 0, sizeof(info));

    for (;;) {
        ssize_t output_index = AMediaCodec_dequeueOutputBuffer(
            g_webrtc_av1_codec,
            &info,
            0
        );

        if (output_index >= 0) {
            int render = info.size > 0 ? 1 : 0;

            media_status_t release_status =
                AMediaCodec_releaseOutputBuffer(
                    g_webrtc_av1_codec,
                    (size_t)output_index,
                    render
                );

            if (release_status != AMEDIA_OK) {
                g_webrtc_av1_drops++;

                LOGE(
                    "WebRTC AV1 AMediaCodec_releaseOutputBuffer failed: "
                    "status=%d drops=%llu",
                    (int)release_status,
                    g_webrtc_av1_drops
                );

                continue;
            }

            if (render) {
                g_webrtc_av1_decoded_frames++;

                if (
                    g_webrtc_av1_decoded_frames == 1 ||
                    g_webrtc_av1_decoded_frames % 120 == 0
                ) {
                    LOGI(
                        "WebRTC AV1 MediaCodec output frame: "
                        "decoded=%llu queued=%llu drops=%llu size=%dx%d fps=%d",
                        g_webrtc_av1_decoded_frames,
                        g_webrtc_av1_input_units,
                        g_webrtc_av1_drops,
                        g_webrtc_av1_width,
                        g_webrtc_av1_height,
                        g_webrtc_av1_fps
                    );
                }
            }
        } else if (
            output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
        ) {
            AMediaFormat *output_format =
                AMediaCodec_getOutputFormat(g_webrtc_av1_codec);

            if (output_format != NULL) {
                const char *description =
                    AMediaFormat_toString(output_format);

                LOGI(
                    "WebRTC AV1 MediaCodec output format changed: %s",
                    description != NULL ? description : "NULL"
                );

                AMediaFormat_delete(output_format);
            }
        } else {
            break;
        }
    }
}

__attribute__((visibility("default")))
void gst_quest_webrtc_av1_stop_surface(void)
{
    AMediaCodec *codec = NULL;
    ANativeWindow *window = NULL;
    jobject surface_global = NULL;

    pthread_mutex_lock(&g_webrtc_av1_codec_mutex);

    codec = g_webrtc_av1_codec;
    window = g_webrtc_av1_window;
    surface_global = g_webrtc_av1_surface_global;

    g_webrtc_av1_codec = NULL;
    g_webrtc_av1_window = NULL;
    g_webrtc_av1_surface_global = NULL;
    g_webrtc_av1_surface_running = 0;
    g_webrtc_av1_pts_us = 0;

    pthread_mutex_unlock(&g_webrtc_av1_codec_mutex);

    if (codec != NULL) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }

    if (window != NULL) {
        ANativeWindow_release(window);
    }

    if (surface_global != NULL) {
        int attached = 0;
        JNIEnv *env = nh265_get_env(&attached);

        if (env != NULL) {
            (*env)->DeleteGlobalRef(env, surface_global);
        }

        nh265_detach_env(attached);
    }

    LOGI(
        "WebRTC AV1 Surface decoder stopped. "
        "queued=%llu decoded=%llu drops=%llu",
        g_webrtc_av1_input_units,
        g_webrtc_av1_decoded_frames,
        g_webrtc_av1_drops
    );
}

__attribute__((visibility("default")))
int gst_quest_webrtc_av1_start_surface(
    void *surface_obj,
    int width,
    int height,
    int fps
)
{
    if (surface_obj == NULL) {
        LOGE("WebRTC AV1 start failed: surface is NULL.");
        return 0;
    }

    gst_quest_webrtc_av1_stop_surface();

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);

    if (env == NULL) {
        LOGE("WebRTC AV1 start failed: JNI env unavailable.");
        return 0;
    }

    jobject surface_global =
        (*env)->NewGlobalRef(env, (jobject)surface_obj);

    if (surface_global == NULL) {
        LOGE(
            "WebRTC AV1 start failed: NewGlobalRef returned NULL."
        );

        nh265_detach_env(attached);
        return 0;
    }

    ANativeWindow *window =
        ANativeWindow_fromSurface(env, surface_global);

    if (window == NULL) {
        LOGE(
            "WebRTC AV1 start failed: "
            "ANativeWindow_fromSurface failed."
        );

        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    AMediaCodec *codec =
        AMediaCodec_createDecoderByType("video/av01");

    if (codec == NULL) {
        LOGE(
            "WebRTC AV1 start failed: "
            "AMediaCodec_createDecoderByType(video/av01) failed."
        );

        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    int configured_width = width > 0 ? width : 4096;
    int configured_height = height > 0 ? height : 2048;
    int configured_fps = fps > 0 ? fps : 120;

    AMediaFormat *format = AMediaFormat_new();

    if (format == NULL) {
        LOGE("WebRTC AV1 start failed: AMediaFormat_new failed.");

        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    AMediaFormat_setString(
        format,
        AMEDIAFORMAT_KEY_MIME,
        "video/av01"
    );

    AMediaFormat_setInt32(
        format,
        AMEDIAFORMAT_KEY_WIDTH,
        configured_width
    );

    AMediaFormat_setInt32(
        format,
        AMEDIAFORMAT_KEY_HEIGHT,
        configured_height
    );

    AMediaFormat_setInt32(
        format,
        "frame-rate",
        configured_fps
    );

    AMediaFormat_setInt32(
        format,
        "max-input-size",
        8 * 1024 * 1024
    );

    const char *format_description =
        AMediaFormat_toString(format);

    LOGI(
        "WebRTC AV1 configuring MediaCodec: %s",
        format_description != NULL ? format_description : "NULL"
    );

    media_status_t status =
        AMediaCodec_configure(codec, format, window, NULL, 0);

    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        LOGE(
            "WebRTC AV1 AMediaCodec_configure failed: %d",
            (int)status
        );

        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    status = AMediaCodec_start(codec);

    if (status != AMEDIA_OK) {
        LOGE(
            "WebRTC AV1 AMediaCodec_start failed: %d",
            (int)status
        );

        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        return 0;
    }

    pthread_mutex_lock(&g_webrtc_av1_codec_mutex);

    g_webrtc_av1_codec = codec;
    g_webrtc_av1_window = window;
    g_webrtc_av1_surface_global = surface_global;

    g_webrtc_av1_width = configured_width;
    g_webrtc_av1_height = configured_height;
    g_webrtc_av1_fps = configured_fps;

    g_webrtc_av1_pts_us = 0;
    g_webrtc_av1_input_units = 0;
    g_webrtc_av1_decoded_frames = 0;
    g_webrtc_av1_drops = 0;

    g_webrtc_av1_surface_running = 1;

    pthread_mutex_unlock(&g_webrtc_av1_codec_mutex);

    nh265_detach_env(attached);

    LOGI(
        "WebRTC AV1 Surface decoder started size=%dx%d fps=%d "
        "mime=video/av01",
        configured_width,
        configured_height,
        configured_fps
    );

    return 1;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_av1_get_running(void)
{
    return g_webrtc_av1_surface_running ? 1 : 0;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_av1_get_input_units(void)
{
    return g_webrtc_av1_input_units;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_av1_get_decoded_frames(void)
{
    return g_webrtc_av1_decoded_frames;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_av1_get_drops(void)
{
    return g_webrtc_av1_drops;
}

static int webrtc_av1_queue_encoded_gst_buffer(GstBuffer *buffer)
{
    if (buffer == NULL) {
        return 0;
    }

    GstMapInfo map;
    memset(&map, 0, sizeof(map));

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        LOGE("WebRTC AV1 could not map encoded GstBuffer.");
        return 0;
    }

    int queued = 0;

    pthread_mutex_lock(&g_webrtc_av1_codec_mutex);

    if (
        g_webrtc_av1_surface_running &&
        g_webrtc_av1_codec != NULL &&
        map.data != NULL &&
        map.size > 0
    ) {
        webrtc_av1_drain_codec_locked();

        ssize_t input_index = AMediaCodec_dequeueInputBuffer(
            g_webrtc_av1_codec,
            0
        );

        if (input_index >= 0) {
            size_t input_capacity = 0;

            uint8_t *input_buffer = AMediaCodec_getInputBuffer(
                g_webrtc_av1_codec,
                (size_t)input_index,
                &input_capacity
            );

            if (
                input_buffer == NULL ||
                input_capacity < map.size
            ) {
                g_webrtc_av1_drops++;

                LOGE(
                    "WebRTC AV1 MediaCodec input buffer too small: "
                    "required=%lu capacity=%lu drops=%llu",
                    (unsigned long)map.size,
                    (unsigned long)input_capacity,
                    g_webrtc_av1_drops
                );

                AMediaCodec_queueInputBuffer(
                    g_webrtc_av1_codec,
                    (size_t)input_index,
                    0,
                    0,
                    g_webrtc_av1_pts_us,
                    0
                );
            } else {
                memcpy(input_buffer, map.data, map.size);

                media_status_t status =
                    AMediaCodec_queueInputBuffer(
                        g_webrtc_av1_codec,
                        (size_t)input_index,
                        0,
                        map.size,
                        g_webrtc_av1_pts_us,
                        0
                    );

                if (status == AMEDIA_OK) {
                    int step =
                        g_webrtc_av1_fps > 0
                            ? (1000000 / g_webrtc_av1_fps)
                            : 8333;

                    g_webrtc_av1_pts_us += step;
                    g_webrtc_av1_input_units++;
                    queued = 1;

                    if (
                        g_webrtc_av1_input_units == 1 ||
                        g_webrtc_av1_input_units % 120 == 0
                    ) {
                        LOGI(
                            "WebRTC AV1 queued temporal unit to MediaCodec: "
                            "unit=%llu bytes=%lu size=%dx%d fps=%d",
                            g_webrtc_av1_input_units,
                            (unsigned long)map.size,
                            g_webrtc_av1_width,
                            g_webrtc_av1_height,
                            g_webrtc_av1_fps
                        );
                    }
                } else {
                    g_webrtc_av1_drops++;

                    LOGE(
                        "WebRTC AV1 AMediaCodec_queueInputBuffer failed: "
                        "status=%d bytes=%lu drops=%llu",
                        (int)status,
                        (unsigned long)map.size,
                        g_webrtc_av1_drops
                    );
                }
            }

            webrtc_av1_drain_codec_locked();
        } else {
            g_webrtc_av1_drops++;

            if (
                g_webrtc_av1_drops == 1 ||
                g_webrtc_av1_drops % 60 == 0
            ) {
                LOGE(
                    "WebRTC AV1 no MediaCodec input buffer available: "
                    "result=%ld drops=%llu",
                    (long)input_index,
                    g_webrtc_av1_drops
                );
            }
        }
    } else {
        g_webrtc_av1_drops++;

        if (
            g_webrtc_av1_drops == 1 ||
            g_webrtc_av1_drops % 120 == 0
        ) {
            LOGE(
                "WebRTC AV1 Surface decoder not ready; "
                "dropping temporal unit drops=%llu bytes=%lu "
                "running=%d codec=%p",
                g_webrtc_av1_drops,
                (unsigned long)map.size,
                g_webrtc_av1_surface_running,
                g_webrtc_av1_codec
            );
        }
    }

    pthread_mutex_unlock(&g_webrtc_av1_codec_mutex);

    gst_buffer_unmap(buffer, &map);
    return queued;
}

/* ================= WEBRTC_AV1_SURFACE_AMEDIACODEC_END ================= */


/* ================= WEBRTC_H264_SURFACE_AMEDIACODEC_START ================= */

static pthread_mutex_t g_webrtc_h264_codec_mutex = PTHREAD_MUTEX_INITIALIZER;
static AMediaCodec *g_webrtc_h264_codec = NULL;
static ANativeWindow *g_webrtc_h264_window = NULL;
static jobject g_webrtc_h264_surface_global = NULL;
static volatile int g_webrtc_h264_surface_running = 0;
static int g_webrtc_h264_width = 4096;
static int g_webrtc_h264_height = 2048;
static int g_webrtc_h264_fps = 60;
static int64_t g_webrtc_h264_pts_us = 0;
static unsigned long long g_webrtc_h264_input_aus = 0;
static unsigned long long g_webrtc_h264_decoded_frames = 0;
static unsigned long long g_webrtc_h264_drops = 0;

static void webrtc_h264_drain_codec_locked(void)
{
    if (g_webrtc_h264_codec == NULL) return;

    AMediaCodecBufferInfo info;
    for (;;) {
        ssize_t output_index = AMediaCodec_dequeueOutputBuffer(
            g_webrtc_h264_codec,
            &info,
            0
        );

        if (output_index >= 0) {
            media_status_t status = AMediaCodec_releaseOutputBuffer(
                g_webrtc_h264_codec,
                (size_t)output_index,
                info.size > 0
            );

            if (status == AMEDIA_OK && info.size > 0) {
                g_webrtc_h264_decoded_frames++;
            } else if (status != AMEDIA_OK) {
                g_webrtc_h264_drops++;
            }
        } else if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(
                g_webrtc_h264_codec
            );
            if (format != NULL) {
                const char *description = AMediaFormat_toString(format);
                LOGI(
                    "WebRTC H264 MediaCodec output format changed: %s",
                    description != NULL ? description : "NULL"
                );
                AMediaFormat_delete(format);
            }
        } else {
            break;
        }
    }
}

__attribute__((visibility("default")))
void gst_quest_webrtc_h264_stop_surface(void)
{
    AMediaCodec *codec = NULL;
    ANativeWindow *window = NULL;
    jobject surface_global = NULL;

    pthread_mutex_lock(&g_webrtc_h264_codec_mutex);
    codec = g_webrtc_h264_codec;
    window = g_webrtc_h264_window;
    surface_global = g_webrtc_h264_surface_global;

    g_webrtc_h264_codec = NULL;
    g_webrtc_h264_window = NULL;
    g_webrtc_h264_surface_global = NULL;
    g_webrtc_h264_surface_running = 0;
    g_webrtc_h264_pts_us = 0;
    pthread_mutex_unlock(&g_webrtc_h264_codec_mutex);

    if (codec != NULL) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (window != NULL) ANativeWindow_release(window);

    if (surface_global != NULL) {
        int attached = 0;
        JNIEnv *env = nh265_get_env(&attached);
        if (env != NULL) (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
    }

    LOGI(
        "WebRTC H264 Surface decoder stopped queued=%llu decoded=%llu drops=%llu",
        g_webrtc_h264_input_aus,
        g_webrtc_h264_decoded_frames,
        g_webrtc_h264_drops
    );
}

__attribute__((visibility("default")))
int gst_quest_webrtc_h264_start_surface(
    void *surface_obj,
    int width,
    int height,
    int fps
)
{
    if (surface_obj == NULL) {
        LOGE("WebRTC H264 start failed: surface is NULL.");
        return 0;
    }

    gst_quest_webrtc_h264_stop_surface();

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);
    if (env == NULL) {
        LOGE("WebRTC H264 start failed: JNI env unavailable.");
        return 0;
    }

    jobject surface_global = (*env)->NewGlobalRef(env, (jobject)surface_obj);
    if (surface_global == NULL) {
        nh265_detach_env(attached);
        LOGE("WebRTC H264 start failed: NewGlobalRef returned NULL.");
        return 0;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface_global);
    if (window == NULL) {
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        LOGE("WebRTC H264 start failed: ANativeWindow_fromSurface failed.");
        return 0;
    }

    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");
    if (codec == NULL) {
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        LOGE("WebRTC H264 start failed: createDecoderByType(video/avc) failed.");
        return 0;
    }

    int configured_width = width > 0 ? width : 4096;
    int configured_height = height > 0 ? height : 2048;
    int configured_fps = fps > 0 ? fps : 60;

    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, configured_width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, configured_height);
    AMediaFormat_setInt32(format, "frame-rate", configured_fps);
    AMediaFormat_setInt32(format, "max-input-size", 8 * 1024 * 1024);

    const char *description = AMediaFormat_toString(format);
    LOGI(
        "WebRTC H264 configuring MediaCodec: %s",
        description != NULL ? description : "NULL"
    );

    media_status_t status = AMediaCodec_configure(
        codec,
        format,
        window,
        NULL,
        0
    );
    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        LOGE("WebRTC H264 AMediaCodec_configure failed: %d", (int)status);
        return 0;
    }

    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
        LOGE("WebRTC H264 AMediaCodec_start failed: %d", (int)status);
        return 0;
    }

    pthread_mutex_lock(&g_webrtc_h264_codec_mutex);
    g_webrtc_h264_codec = codec;
    g_webrtc_h264_window = window;
    g_webrtc_h264_surface_global = surface_global;
    g_webrtc_h264_width = configured_width;
    g_webrtc_h264_height = configured_height;
    g_webrtc_h264_fps = configured_fps;
    g_webrtc_h264_pts_us = 0;
    g_webrtc_h264_input_aus = 0;
    g_webrtc_h264_decoded_frames = 0;
    g_webrtc_h264_drops = 0;
    g_webrtc_h264_surface_running = 1;
    pthread_mutex_unlock(&g_webrtc_h264_codec_mutex);

    nh265_detach_env(attached);

    LOGI(
        "WebRTC H264 Surface decoder started size=%dx%d fps=%d mime=video/avc",
        configured_width,
        configured_height,
        configured_fps
    );
    return 1;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_h264_get_running(void)
{
    return g_webrtc_h264_surface_running ? 1 : 0;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h264_get_input_units(void)
{
    return g_webrtc_h264_input_aus;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h264_get_decoded_frames(void)
{
    return g_webrtc_h264_decoded_frames;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_h264_get_drops(void)
{
    return g_webrtc_h264_drops;
}

static int webrtc_h264_queue_encoded_gst_buffer(GstBuffer *buffer)
{
    if (buffer == NULL) return 0;

    GstMapInfo map;
    memset(&map, 0, sizeof(map));
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        LOGE("WebRTC H264 could not map encoded GstBuffer.");
        return 0;
    }

    int queued = 0;
    pthread_mutex_lock(&g_webrtc_h264_codec_mutex);

    if (
        g_webrtc_h264_surface_running &&
        g_webrtc_h264_codec != NULL &&
        map.data != NULL &&
        map.size > 0
    ) {
        webrtc_h264_drain_codec_locked();

        ssize_t input_index = AMediaCodec_dequeueInputBuffer(
            g_webrtc_h264_codec,
            1000
        );

        if (input_index >= 0) {
            size_t input_capacity = 0;
            uint8_t *input_buffer = AMediaCodec_getInputBuffer(
                g_webrtc_h264_codec,
                (size_t)input_index,
                &input_capacity
            );

            if (input_buffer == NULL || input_capacity < map.size) {
                g_webrtc_h264_drops++;
                AMediaCodec_queueInputBuffer(
                    g_webrtc_h264_codec,
                    (size_t)input_index,
                    0,
                    0,
                    g_webrtc_h264_pts_us,
                    0
                );
            } else {
                memcpy(input_buffer, map.data, map.size);
                media_status_t status = AMediaCodec_queueInputBuffer(
                    g_webrtc_h264_codec,
                    (size_t)input_index,
                    0,
                    map.size,
                    g_webrtc_h264_pts_us,
                    0
                );

                if (status == AMEDIA_OK) {
                    int step = g_webrtc_h264_fps > 0
                        ? (1000000 / g_webrtc_h264_fps)
                        : 16666;
                    g_webrtc_h264_pts_us += step;
                    g_webrtc_h264_input_aus++;
                    queued = 1;
                } else {
                    g_webrtc_h264_drops++;
                }
            }

            webrtc_h264_drain_codec_locked();
        } else {
            g_webrtc_h264_drops++;
        }
    } else {
        g_webrtc_h264_drops++;
    }

    pthread_mutex_unlock(&g_webrtc_h264_codec_mutex);
    gst_buffer_unmap(buffer, &map);
    return queued;
}

/* ================= WEBRTC_H264_SURFACE_AMEDIACODEC_END ================= */

/* ================= QSXR_UNIFIED_WEBRTC_VIDEO_CONTROLLER_START ================= */

static pthread_mutex_t g_qsxr_video_controller_mutex = PTHREAD_MUTEX_INITIALIZER;
static jobject g_qsxr_video_surface_global = NULL;
static int g_qsxr_video_width = 4096;
static int g_qsxr_video_height = 2048;
static int g_qsxr_video_fps = 60;
static int g_qsxr_video_requested_codec = QSXR_CODEC_AUTO;
static int g_qsxr_video_detected_codec = QSXR_CODEC_AUTO;
static int g_qsxr_video_active_codec = QSXR_CODEC_AUTO;
static unsigned long long g_qsxr_video_generation = 0;

static const char *qsxr_codec_name(int codec)
{
    switch (codec) {
        case QSXR_CODEC_H264: return "h264";
        case QSXR_CODEC_H265: return "h265";
        case QSXR_CODEC_AV1: return "av1";
        default: return "auto";
    }
}

static int qsxr_webrtc_video_activate_codec(int codec)
{
    int result = 0;

    pthread_mutex_lock(&g_qsxr_video_controller_mutex);

    if (g_qsxr_video_surface_global == NULL) {
        LOGI(
            "QSXR decoder activation deferred for codec=%s because Surface is not registered yet.",
            qsxr_codec_name(codec)
        );
        pthread_mutex_unlock(&g_qsxr_video_controller_mutex);
        return 0;
    }

    if (
        g_qsxr_video_requested_codec != QSXR_CODEC_AUTO &&
        g_qsxr_video_requested_codec != codec
    ) {
        LOGE(
            "QSXR decoder activation rejected requested=%s incoming=%s",
            qsxr_codec_name(g_qsxr_video_requested_codec),
            qsxr_codec_name(codec)
        );
        pthread_mutex_unlock(&g_qsxr_video_controller_mutex);
        return 0;
    }

    gst_quest_webrtc_h264_stop_surface();
    gst_quest_webrtc_h265_stop_surface();
    gst_quest_webrtc_av1_stop_surface();

    if (codec == QSXR_CODEC_H264) {
        result = gst_quest_webrtc_h264_start_surface(
            g_qsxr_video_surface_global,
            g_qsxr_video_width,
            g_qsxr_video_height,
            g_qsxr_video_fps
        );
    } else if (codec == QSXR_CODEC_H265) {
        result = gst_quest_webrtc_h265_start_surface(
            g_qsxr_video_surface_global,
            g_qsxr_video_width,
            g_qsxr_video_height,
            g_qsxr_video_fps
        );
    } else if (codec == QSXR_CODEC_AV1) {
        result = gst_quest_webrtc_av1_start_surface(
            g_qsxr_video_surface_global,
            g_qsxr_video_width,
            g_qsxr_video_height,
            g_qsxr_video_fps
        );
    }

    g_qsxr_video_active_codec = result ? codec : QSXR_CODEC_AUTO;
    g_qsxr_video_generation++;

    LOGI(
        "QSXR unified decoder activation codec=%s result=%d size=%dx%d fps=%d generation=%llu",
        qsxr_codec_name(codec),
        result,
        g_qsxr_video_width,
        g_qsxr_video_height,
        g_qsxr_video_fps,
        g_qsxr_video_generation
    );

    pthread_mutex_unlock(&g_qsxr_video_controller_mutex);
    return result;
}

static int qsxr_webrtc_video_note_incoming_codec(int codec)
{
    int allowed;
    int has_surface;

    pthread_mutex_lock(&g_qsxr_video_controller_mutex);
    g_qsxr_video_detected_codec = codec;
    g_qsxr_video_generation++;
    allowed =
        g_qsxr_video_requested_codec == QSXR_CODEC_AUTO ||
        g_qsxr_video_requested_codec == codec;
    has_surface = g_qsxr_video_surface_global != NULL;
    pthread_mutex_unlock(&g_qsxr_video_controller_mutex);

    LOGI(
        "QSXR incoming codec detected=%s requested=%s allowed=%d surface=%d",
        qsxr_codec_name(codec),
        qsxr_codec_name(g_qsxr_video_requested_codec),
        allowed,
        has_surface
    );

    if (!allowed) return 0;
    if (has_surface) qsxr_webrtc_video_activate_codec(codec);
    return 1;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_video_register_surface(
    void *surface_obj,
    int width,
    int height,
    int fps,
    int requested_codec
)
{
    if (surface_obj == NULL) {
        LOGE("QSXR register Surface failed: surface is NULL.");
        return 0;
    }

    if (
        requested_codec < QSXR_CODEC_AUTO ||
        requested_codec > QSXR_CODEC_AV1
    ) {
        requested_codec = QSXR_CODEC_AUTO;
    }

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);
    if (env == NULL) {
        LOGE("QSXR register Surface failed: JNI env unavailable.");
        return 0;
    }

    jobject new_surface_global = (*env)->NewGlobalRef(
        env,
        (jobject)surface_obj
    );
    if (new_surface_global == NULL) {
        nh265_detach_env(attached);
        LOGE("QSXR register Surface failed: NewGlobalRef returned NULL.");
        return 0;
    }

    jobject old_surface_global = NULL;
    int target_codec;

    pthread_mutex_lock(&g_qsxr_video_controller_mutex);
    old_surface_global = g_qsxr_video_surface_global;
    g_qsxr_video_surface_global = new_surface_global;
    g_qsxr_video_width = width > 0 ? width : 4096;
    g_qsxr_video_height = height > 0 ? height : 2048;
    g_qsxr_video_fps = fps > 0 ? fps : 60;
    g_qsxr_video_requested_codec = requested_codec;
    target_codec = requested_codec != QSXR_CODEC_AUTO
        ? requested_codec
        : g_qsxr_video_detected_codec;
    g_qsxr_video_generation++;
    pthread_mutex_unlock(&g_qsxr_video_controller_mutex);

    if (old_surface_global != NULL) {
        (*env)->DeleteGlobalRef(env, old_surface_global);
    }
    nh265_detach_env(attached);

    LOGI(
        "QSXR Surface registered requested=%s size=%dx%d fps=%d target=%s",
        qsxr_codec_name(requested_codec),
        g_qsxr_video_width,
        g_qsxr_video_height,
        g_qsxr_video_fps,
        qsxr_codec_name(target_codec)
    );

    if (target_codec != QSXR_CODEC_AUTO) {
        return qsxr_webrtc_video_activate_codec(target_codec);
    }

    return 1;
}

__attribute__((visibility("default")))
void gst_quest_webrtc_video_stop_surface(void)
{
    jobject surface_global = NULL;

    pthread_mutex_lock(&g_qsxr_video_controller_mutex);
    surface_global = g_qsxr_video_surface_global;
    g_qsxr_video_surface_global = NULL;
    g_qsxr_video_detected_codec = QSXR_CODEC_AUTO;
    g_qsxr_video_active_codec = QSXR_CODEC_AUTO;
    g_qsxr_video_generation++;
    pthread_mutex_unlock(&g_qsxr_video_controller_mutex);

    gst_quest_webrtc_h264_stop_surface();
    gst_quest_webrtc_h265_stop_surface();
    gst_quest_webrtc_av1_stop_surface();

    if (surface_global != NULL) {
        int attached = 0;
        JNIEnv *env = nh265_get_env(&attached);
        if (env != NULL) (*env)->DeleteGlobalRef(env, surface_global);
        nh265_detach_env(attached);
    }

    LOGI("QSXR unified WebRTC video Surface stopped.");
}

__attribute__((visibility("default")))
int gst_quest_webrtc_video_get_requested_codec(void)
{
    return g_qsxr_video_requested_codec;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_video_get_detected_codec(void)
{
    return g_qsxr_video_detected_codec;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_video_get_active_codec(void)
{
    return g_qsxr_video_active_codec;
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_video_get_generation(void)
{
    return g_qsxr_video_generation;
}

__attribute__((visibility("default")))
int gst_quest_webrtc_video_get_running(void)
{
    switch (g_qsxr_video_active_codec) {
        case QSXR_CODEC_H264: return gst_quest_webrtc_h264_get_running();
        case QSXR_CODEC_H265: return gst_quest_webrtc_h265_get_running();
        case QSXR_CODEC_AV1: return gst_quest_webrtc_av1_get_running();
        default: return 0;
    }
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_video_get_input_units(void)
{
    switch (g_qsxr_video_active_codec) {
        case QSXR_CODEC_H264: return gst_quest_webrtc_h264_get_input_units();
        case QSXR_CODEC_H265: return gst_quest_webrtc_h265_get_input_units();
        case QSXR_CODEC_AV1: return gst_quest_webrtc_av1_get_input_units();
        default: return 0;
    }
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_video_get_decoded_frames(void)
{
    switch (g_qsxr_video_active_codec) {
        case QSXR_CODEC_H264: return gst_quest_webrtc_h264_get_decoded_frames();
        case QSXR_CODEC_H265: return gst_quest_webrtc_h265_get_decoded_frames();
        case QSXR_CODEC_AV1: return gst_quest_webrtc_av1_get_decoded_frames();
        default: return 0;
    }
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_video_get_drops(void)
{
    switch (g_qsxr_video_active_codec) {
        case QSXR_CODEC_H264: return gst_quest_webrtc_h264_get_drops();
        case QSXR_CODEC_H265: return gst_quest_webrtc_h265_get_drops();
        case QSXR_CODEC_AV1: return gst_quest_webrtc_av1_get_drops();
        default: return 0;
    }
}

__attribute__((visibility("default")))
unsigned long long gst_quest_webrtc_video_get_backlog(void)
{
    unsigned long long input = gst_quest_webrtc_video_get_input_units();
    unsigned long long decoded = gst_quest_webrtc_video_get_decoded_frames();
    return input > decoded ? input - decoded : 0;
}

/* ================= QSXR_UNIFIED_WEBRTC_VIDEO_CONTROLLER_END ================= */

static int nh265_depay_rtp(
    const uint8_t *packet,
    int packet_len,
    NativeH265Buffer *au,
    NativeH265Buffer *fu,
    uint16_t *last_seq,
    int *have_last_seq
)
{
    if (!packet || packet_len < 15 || !au || !fu) return 0;

    int version = (packet[0] >> 6) & 0x03;
    if (version != 2) return 0;

    int csrc_count = packet[0] & 0x0F;
    int extension = (packet[0] & 0x10) != 0;

    int header_len = 12 + csrc_count * 4;
    if (packet_len <= header_len + 2) return 0;

    uint16_t seq = (uint16_t)((packet[2] << 8) | packet[3]);

    if (*have_last_seq) {
        uint16_t expected = (uint16_t)(*last_seq + 1);
        if (seq != expected) {
            nh265_clear(fu);
            nh265_inc_drop();
        }
    }

    *last_seq = seq;
    *have_last_seq = 1;

    if (extension) {
        if (packet_len < header_len + 4) return 0;
        int ext_words = (packet[header_len + 2] << 8) | packet[header_len + 3];
        header_len += 4 + ext_words * 4;
        if (packet_len <= header_len + 2) return 0;
    }

    const uint8_t *payload = packet + header_len;
    int payload_len = packet_len - header_len;

    if (payload_len <= 2) return 0;

    uint8_t b0 = payload[0];
    uint8_t b1 = payload[1];
    int nal_type = (b0 >> 1) & 0x3F;

    if (nal_type >= 0 && nal_type <= 47) {
        return nh265_append_nal(au, payload, (size_t)payload_len);
    }

    if (nal_type == 48) {
        int pos = 2;
        int appended = 0;

        while (pos + 2 <= payload_len) {
            int size = (payload[pos] << 8) | payload[pos + 1];
            pos += 2;

            if (size <= 2 || pos + size > payload_len) break;

            if (nh265_append_nal(au, payload + pos, (size_t)size)) {
                appended++;
            }

            pos += size;
        }

        return appended > 0;
    }

    if (nal_type == 49) {
        if (payload_len < 3) return 0;

        uint8_t fu_indicator0 = payload[0];
        uint8_t fu_indicator1 = payload[1];
        uint8_t fu_header = payload[2];

        int start = (fu_header & 0x80) != 0;
        int end = (fu_header & 0x40) != 0;
        int original_type = fu_header & 0x3F;

        uint8_t reconstructed0 = (uint8_t)((fu_indicator0 & 0x81) | (original_type << 1));
        uint8_t reconstructed1 = fu_indicator1;

        if (start) {
            nh265_clear(fu);
            uint8_t hdr[2] = { reconstructed0, reconstructed1 };
            nh265_append(fu, hdr, 2);
            nh265_append(fu, payload + 3, (size_t)(payload_len - 3));
        } else {
            if (fu->size == 0) return 0;
            nh265_append(fu, payload + 3, (size_t)(payload_len - 3));
        }

        if (end && fu->size > 2) {
            int ok = nh265_append_nal(au, fu->data, fu->size);
            nh265_clear(fu);
            return ok;
        }
    }

    return 0;
}

static void* nh265_thread_main(void *arg)
{
    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);

    if (!env) {
        NH265_LOGE("Could not get JNI env.");
        g_native_h265_running = 0;
        return NULL;
    }

    if (!g_native_h265_surface_global) {
        NH265_LOGE("Surface global ref is NULL.");
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, g_native_h265_surface_global);

    if (!window) {
        NH265_LOGE("ANativeWindow_fromSurface failed.");
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/hevc");

    if (!codec) {
        NH265_LOGE("AMediaCodec_createDecoderByType(video/hevc) failed.");
        ANativeWindow_release(window);
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, g_native_h265_width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, g_native_h265_height);
    AMediaFormat_setInt32(fmt, "frame-rate", g_native_h265_fps);
    AMediaFormat_setInt32(fmt, "max-input-size", 4 * 1024 * 1024);

    static const uint8_t hevc_csd0[] = {
        0x00,0x00,0x00,0x01,
        0x40,0x01,0x0c,0x01,0xff,0xff,0x01,0x60,0x00,0x00,0x03,0x00,0x90,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x96,0x97,0x02,0x40,
        0x00,0x00,0x00,0x01,
        0x42,0x01,0x01,0x01,0x60,0x00,0x00,0x03,0x00,0x90,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x96,0xa0,0x00,0x80,0x08,0x00,0x80,0x16,0x59,0x74,0xa4,0x21,0x19,0x17,0xfe,0x30,0x16,0xa0,0x20,0x20,0x20,0x80,0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x0f,0x04,
        0x00,0x00,0x00,0x01,
        0x44,0x01,0xc0,0x93,0x7c,0x0c,0xc9
    };

    AMediaFormat_setBuffer(fmt, "csd-0", hevc_csd0, sizeof(hevc_csd0));

    media_status_t st = AMediaCodec_configure(codec, fmt, window, NULL, 0);
    AMediaFormat_delete(fmt);

    if (st != AMEDIA_OK) {
        NH265_LOGE("AMediaCodec_configure failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    st = AMediaCodec_start(codec);

    if (st != AMEDIA_OK) {
        NH265_LOGE("AMediaCodec_start failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        NH265_LOGE("socket failed errno=%d", errno);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_native_h265_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        NH265_LOGE("bind UDP port %d failed errno=%d", g_native_h265_port, errno);
        close(sock);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh265_detach_env(attached);
        g_native_h265_running = 0;
        return NULL;
    }

    NH265_LOGI("Native H265 RTP receiver started port=%d size=%dx%d fps=%d",
        g_native_h265_port, g_native_h265_width, g_native_h265_height, g_native_h265_fps);

    NativeH265Buffer au = {0};
    NativeH265Buffer fu = {0};
    uint8_t packet[65536];
    uint16_t last_seq = 0;
    int have_last_seq = 0;
    int64_t pts_us = 0;

    while (!g_native_h265_stop) {
        ssize_t n = recv(sock, packet, sizeof(packet), 0);

        if (n > 0) {
            nh265_inc_rtp();

            int marker = (packet[1] & 0x80) != 0;

            nh265_depay_rtp(packet, (int)n, &au, &fu, &last_seq, &have_last_seq);

            if (marker && au.size > 0) {
                nh265_queue_access_unit(codec, &au, g_native_h265_fps, &pts_us);
                nh265_clear(&au);
            }
        }

        nh265_drain_codec(codec);
    }

    nh265_drain_codec(codec);

    nh265_free(&au);
    nh265_free(&fu);

    close(sock);

    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    ANativeWindow_release(window);

    NH265_LOGI("Native H265 RTP receiver stopped.");

    nh265_detach_env(attached);
    g_native_h265_running = 0;
    return NULL;
}

__attribute__((visibility("default"))) int gst_quest_native_h265_start(void *surface_obj, int width, int height, int port, int fps)
{
    if (g_native_h265_running) {
        NH265_LOGI("Native H265 receiver already running.");
        return 1;
    }

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);

    if (!env) {
        NH265_LOGE("native_h265_start failed: no JNI env.");
        return 0;
    }

    if (surface_obj == NULL) {
        NH265_LOGE("native_h265_start failed: surface is NULL.");
        nh265_detach_env(attached);
        return 0;
    }

    if (g_native_h265_surface_global != NULL) {
        (*env)->DeleteGlobalRef(env, g_native_h265_surface_global);
        g_native_h265_surface_global = NULL;
    }

    g_native_h265_surface_global = (*env)->NewGlobalRef(env, (jobject)surface_obj);

    g_native_h265_width = width;
    g_native_h265_height = height;
    g_native_h265_port = port;
    g_native_h265_fps = fps > 0 ? fps : 60;

    pthread_mutex_lock(&g_native_h265_stats_mutex);
    g_native_h265_rtp_packets = 0;
    g_native_h265_access_units = 0;
    g_native_h265_decoded_frames = 0;
    g_native_h265_dropped_packets = 0;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);

    g_native_h265_stop = 0;
    g_native_h265_running = 1;

    int rc = pthread_create(&g_native_h265_thread, NULL, nh265_thread_main, NULL);

    if (rc != 0) {
        g_native_h265_running = 0;
        NH265_LOGE("pthread_create failed rc=%d", rc);
        nh265_detach_env(attached);
        return 0;
    }

    nh265_detach_env(attached);
    return 1;
}

__attribute__((visibility("default"))) void gst_quest_native_h265_stop(void)
{
    if (!g_native_h265_running) return;

    g_native_h265_stop = 1;
    pthread_join(g_native_h265_thread, NULL);

    int attached = 0;
    JNIEnv *env = nh265_get_env(&attached);

    if (env && g_native_h265_surface_global != NULL) {
        (*env)->DeleteGlobalRef(env, g_native_h265_surface_global);
        g_native_h265_surface_global = NULL;
    }

    nh265_detach_env(attached);
}

__attribute__((visibility("default"))) int gst_quest_native_h265_get_running(void)
{
    return g_native_h265_running;
}

__attribute__((visibility("default"))) int gst_quest_native_h265_get_rtp_packets(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    unsigned long long v = g_native_h265_rtp_packets;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h265_get_access_units(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    unsigned long long v = g_native_h265_access_units;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h265_get_decoded_frames(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    unsigned long long v = g_native_h265_decoded_frames;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h265_get_dropped_packets(void)
{
    pthread_mutex_lock(&g_native_h265_stats_mutex);
    unsigned long long v = g_native_h265_dropped_packets;
    pthread_mutex_unlock(&g_native_h265_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}


/* ================= QUEST_NATIVE_H264_RTP_AMEDIACODEC_RECEIVER ================= */

#define NH264_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GstQuestNativeH264", __VA_ARGS__)
#define NH264_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GstQuestNativeH264", __VA_ARGS__)

static pthread_t g_native_h264_thread;
static volatile int g_native_h264_running = 0;
static volatile int g_native_h264_stop = 0;

static jobject g_native_h264_surface_global = NULL;
static int g_native_h264_width = 4096;
static int g_native_h264_height = 2048;
static int g_native_h264_port = 5004;
static int g_native_h264_fps = 60;

static pthread_mutex_t g_native_h264_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_native_h264_rtp_packets = 0;
static unsigned long long g_native_h264_access_units = 0;
static unsigned long long g_native_h264_decoded_frames = 0;
static unsigned long long g_native_h264_dropped_packets = 0;

typedef struct NativeH264Buffer {
    uint8_t *data;
    size_t size;
    size_t cap;
} NativeH264Buffer;

static int nh264_reserve(NativeH264Buffer *b, size_t extra)
{
    if (!b) return 0;
    size_t need = b->size + extra;
    if (need <= b->cap) return 1;

    size_t new_cap = b->cap ? b->cap : 65536;
    while (new_cap < need) new_cap *= 2;

    uint8_t *p = (uint8_t*)realloc(b->data, new_cap);
    if (!p) return 0;

    b->data = p;
    b->cap = new_cap;
    return 1;
}

static int nh264_append(NativeH264Buffer *b, const uint8_t *src, size_t len)
{
    if (!b || !src || len == 0) return 0;
    if (!nh264_reserve(b, len)) return 0;
    memcpy(b->data + b->size, src, len);
    b->size += len;
    return 1;
}

static int nh264_append_start_code(NativeH264Buffer *b)
{
    static const uint8_t sc[4] = {0,0,0,1};
    return nh264_append(b, sc, 4);
}

static int nh264_append_nal(NativeH264Buffer *au, const uint8_t *nal, size_t nal_len)
{
    if (!au || !nal || nal_len == 0) return 0;
    if (!nh264_append_start_code(au)) return 0;
    return nh264_append(au, nal, nal_len);
}

static void nh264_clear(NativeH264Buffer *b)
{
    if (b) b->size = 0;
}

static void nh264_free(NativeH264Buffer *b)
{
    if (!b) return;
    if (b->data) free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

static void nh264_inc_rtp(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    g_native_h264_rtp_packets++;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
}

static void nh264_inc_drop(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    g_native_h264_dropped_packets++;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
}

static void nh264_inc_au(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    g_native_h264_access_units++;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
}

static void nh264_inc_decoded(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    g_native_h264_decoded_frames++;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
}

static JNIEnv* nh264_get_env(int *attached)
{
    if (attached) *attached = 0;

    if (g_quest_java_vm == NULL) {
        NH264_LOGE("JavaVM is NULL.");
        return NULL;
    }

    JNIEnv *env = NULL;
    jint r = (*g_quest_java_vm)->GetEnv(g_quest_java_vm, (void**)&env, JNI_VERSION_1_6);

    if (r == JNI_OK) return env;

    if (r == JNI_EDETACHED) {
        if ((*g_quest_java_vm)->AttachCurrentThread(g_quest_java_vm, &env, NULL) == JNI_OK) {
            if (attached) *attached = 1;
            return env;
        }
    }

    return NULL;
}

static void nh264_detach_env(int attached)
{
    if (attached && g_quest_java_vm != NULL) {
        (*g_quest_java_vm)->DetachCurrentThread(g_quest_java_vm);
    }
}

static int nh264_queue_access_unit(AMediaCodec *codec, NativeH264Buffer *au, int fps, int64_t *pts_us)
{
    if (!codec || !au || au->size == 0) return 0;

    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 1000);
    if (idx < 0) return 0;

    size_t input_size = 0;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)idx, &input_size);

    if (!input || input_size < au->size) {
        nh264_inc_drop();
        return 0;
    }

    memcpy(input, au->data, au->size);

    media_status_t st = AMediaCodec_queueInputBuffer(codec, (size_t)idx, 0, au->size, *pts_us, 0);

    if (st != AMEDIA_OK) {
        nh264_inc_drop();
        return 0;
    }

    int step = fps > 0 ? (1000000 / fps) : 8333;
    *pts_us += step;
    nh264_inc_au();
    return 1;
}

static void nh264_drain_codec(AMediaCodec *codec)
{
    if (!codec) return;

    AMediaCodecBufferInfo info;

    for (;;) {
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);

        if (out >= 0) {
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out, info.size > 0);
            if (info.size > 0) {
                nh264_inc_decoded();
            }
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
            if (fmt) {
                const char *s = AMediaFormat_toString(fmt);
                NH264_LOGI("Output format changed: %s", s ? s : "NULL");
                AMediaFormat_delete(fmt);
            }
        } else {
            break;
        }
    }
}

typedef struct NativeH264RtpState {
    uint16_t last_seq;
    int have_last_seq;

    uint32_t current_ts;
    int have_current_ts;

    int au_corrupt;
    int wait_for_idr;

    int fu_started;
    uint32_t fu_ts;
    uint16_t fu_last_seq;
    int fu_type;

    unsigned int gap_count;
    unsigned int dropped_aus;
    unsigned int recovered_idr;
} NativeH264RtpState;

static int nh264_seq_next(uint16_t prev, uint16_t cur)
{
    return cur == (uint16_t)(prev + 1);
}

static int nh264_is_vcl_type(int t)
{
    return t >= 1 && t <= 5;
}

static int nh264_is_idr_type(int t)
{
    return t == 5;
}

static int nh264_is_config_type(int t)
{
    return t == 7 || t == 8 || t == 9;
}

static void nh264_drop_current_au(NativeH264Buffer *au, NativeH264Buffer *fu, NativeH264RtpState *st, const char *reason)
{
    if (au) nh264_clear(au);
    if (fu) nh264_clear(fu);

    if (st) {
        st->au_corrupt = 1;
        st->wait_for_idr = 1;
        st->fu_started = 0;
        st->dropped_aus++;
    }

    nh264_inc_drop();

    if (reason && st && (st->dropped_aus <= 10 || (st->dropped_aus % 60) == 0)) {
        NH264_LOGI("H264 robust drop: %s | dropped_aus=%u gaps=%u wait_for_idr=%d",
            reason, st->dropped_aus, st->gap_count, st->wait_for_idr);
    }
}

static int nh264_accept_nal_for_recovery(NativeH264RtpState *st, int nal_type)
{
    if (!st) return 0;

    if (!st->wait_for_idr) {
        return 1;
    }

    if (nh264_is_config_type(nal_type)) {
        return 1;
    }

    if (nh264_is_idr_type(nal_type)) {
        st->wait_for_idr = 0;
        st->au_corrupt = 0;
        st->recovered_idr++;

        NH264_LOGI("H264 robust recovery: accepted IDR, recovered_idr=%u",
            st->recovered_idr);

        return 1;
    }

    return 0;
}

static int nh264_parse_rtp_header(
    const uint8_t *packet,
    int packet_len,
    int *payload_offset,
    int *payload_len,
    uint16_t *seq,
    uint32_t *ts,
    int *marker
)
{
    if (!packet || packet_len < 12 || !payload_offset || !payload_len || !seq || !ts || !marker) return 0;

    int version = (packet[0] >> 6) & 0x03;
    if (version != 2) return 0;

    int padding = (packet[0] & 0x20) != 0;
    int extension = (packet[0] & 0x10) != 0;
    int csrc_count = packet[0] & 0x0F;

    int header_len = 12 + csrc_count * 4;
    if (packet_len <= header_len) return 0;

    if (extension) {
        if (packet_len < header_len + 4) return 0;

        int ext_words = (packet[header_len + 2] << 8) | packet[header_len + 3];
        header_len += 4 + ext_words * 4;

        if (packet_len <= header_len) return 0;
    }

    int plen = packet_len - header_len;

    if (padding) {
        int pad = packet[packet_len - 1];
        if (pad <= 0 || pad > plen) return 0;
        plen -= pad;
    }

    if (plen <= 0) return 0;

    *payload_offset = header_len;
    *payload_len = plen;
    *seq = (uint16_t)((packet[2] << 8) | packet[3]);
    *ts = ((uint32_t)packet[4] << 24) |
          ((uint32_t)packet[5] << 16) |
          ((uint32_t)packet[6] << 8) |
          ((uint32_t)packet[7]);
    *marker = (packet[1] & 0x80) != 0;

    return 1;
}

static int nh264_depay_rtp(
    const uint8_t *packet,
    int packet_len,
    NativeH264Buffer *au,
    NativeH264Buffer *fu,
    NativeH264RtpState *st,
    int *out_marker
)
{
    if (out_marker) *out_marker = 0;
    if (!packet || packet_len < 13 || !au || !fu || !st) return 0;

    int payload_offset = 0;
    int payload_len = 0;
    uint16_t seq = 0;
    uint32_t ts = 0;
    int marker = 0;

    if (!nh264_parse_rtp_header(packet, packet_len, &payload_offset, &payload_len, &seq, &ts, &marker)) {
        nh264_inc_drop();
        return 0;
    }

    if (out_marker) *out_marker = marker;

    if (st->have_last_seq && !nh264_seq_next(st->last_seq, seq)) {
        st->gap_count++;
        nh264_drop_current_au(au, fu, st, "RTP sequence gap");
    }

    st->last_seq = seq;
    st->have_last_seq = 1;

    if (st->have_current_ts && ts != st->current_ts) {
        if (au->size > 0 || fu->size > 0 || st->fu_started) {
            nh264_drop_current_au(au, fu, st, "timestamp changed before marker");
        }

        st->au_corrupt = 0;
        st->current_ts = ts;
    } else if (!st->have_current_ts) {
        st->current_ts = ts;
        st->have_current_ts = 1;
    }

    const uint8_t *payload = packet + payload_offset;

    if (payload_len <= 0) return 0;

    int nal_type = payload[0] & 0x1F;

    if (nal_type >= 1 && nal_type <= 23) {
        if (!nh264_accept_nal_for_recovery(st, nal_type)) {
            if (marker) {
                nh264_clear(au);
                st->au_corrupt = 0;
            }
            return 0;
        }

        return nh264_append_nal(au, payload, (size_t)payload_len);
    }

    if (nal_type == 24) {
        int pos = 1;
        int appended = 0;

        while (pos + 2 <= payload_len) {
            int size = (payload[pos] << 8) | payload[pos + 1];
            pos += 2;

            if (size <= 0 || pos + size > payload_len) {
                nh264_drop_current_au(au, fu, st, "invalid STAP-A size");
                return 0;
            }

            int inner_type = payload[pos] & 0x1F;

            if (nh264_accept_nal_for_recovery(st, inner_type)) {
                if (nh264_append_nal(au, payload + pos, (size_t)size)) {
                    appended++;
                }
            }

            pos += size;
        }

        return appended > 0;
    }

    if (nal_type == 28) {
        if (payload_len < 2) {
            nh264_drop_current_au(au, fu, st, "short FU-A");
            return 0;
        }

        uint8_t fu_indicator = payload[0];
        uint8_t fu_header = payload[1];

        int start = (fu_header & 0x80) != 0;
        int end = (fu_header & 0x40) != 0;
        int reserved = (fu_header & 0x20) != 0;
        int original_type = fu_header & 0x1F;

        if (reserved || (start && end) || original_type == 0 || original_type > 23) {
            nh264_drop_current_au(au, fu, st, "invalid FU-A header");
            return 0;
        }

        if (!nh264_accept_nal_for_recovery(st, original_type)) {
            if (marker) {
                nh264_clear(au);
                nh264_clear(fu);
                st->fu_started = 0;
                st->au_corrupt = 0;
            }
            return 0;
        }

        uint8_t reconstructed = (uint8_t)((fu_indicator & 0xE0) | original_type);

        if (start) {
            nh264_clear(fu);

            if (!nh264_append(fu, &reconstructed, 1)) {
                nh264_drop_current_au(au, fu, st, "FU-A allocation failed at header");
                return 0;
            }

            if (!nh264_append(fu, payload + 2, (size_t)(payload_len - 2))) {
                nh264_drop_current_au(au, fu, st, "FU-A allocation failed at start payload");
                return 0;
            }

            st->fu_started = 1;
            st->fu_ts = ts;
            st->fu_last_seq = seq;
            st->fu_type = original_type;

            return 1;
        }

        if (!st->fu_started || fu->size == 0) {
            nh264_drop_current_au(au, fu, st, "FU-A continuation without start");
            return 0;
        }

        if (ts != st->fu_ts || !nh264_seq_next(st->fu_last_seq, seq)) {
            nh264_drop_current_au(au, fu, st, "FU-A discontinuity");
            return 0;
        }

        if (!nh264_append(fu, payload + 2, (size_t)(payload_len - 2))) {
            nh264_drop_current_au(au, fu, st, "FU-A allocation failed at continuation");
            return 0;
        }

        st->fu_last_seq = seq;

        if (end) {
            int ok = nh264_append_nal(au, fu->data, fu->size);
            nh264_clear(fu);
            st->fu_started = 0;
            return ok;
        }

        return 1;
    }

    nh264_inc_drop();
    return 0;
}

static void* nh264_thread_main(void *arg)
{
    int attached = 0;
    JNIEnv *env = nh264_get_env(&attached);

    if (!env) {
        NH264_LOGE("Could not get JNI env.");
        g_native_h264_running = 0;
        return NULL;
    }

    if (!g_native_h264_surface_global) {
        NH264_LOGE("Surface global ref is NULL.");
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, g_native_h264_surface_global);

    if (!window) {
        NH264_LOGE("ANativeWindow_fromSurface failed.");
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");

    if (!codec) {
        NH264_LOGE("AMediaCodec_createDecoderByType(video/avc) failed.");
        ANativeWindow_release(window);
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, g_native_h264_width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, g_native_h264_height);
    AMediaFormat_setInt32(fmt, "frame-rate", g_native_h264_fps);
    AMediaFormat_setInt32(fmt, "max-input-size", 4 * 1024 * 1024);

    static const uint8_t h264_csd0[] = {
        0x67,0x42,0xc0,0x34,0x95,0x90,0x01,0x00,0x01,0x01,0xb0,0x16,0xa0,
        0x20,0x20,0x28,0x00,0x00,0x1f,0x40,0x00,0x0e,0xa6,0x04,0x20
    };

    static const uint8_t h264_csd1[] = {
        0x68,0xcb,0x8f,0x20
    };

    AMediaFormat_setBuffer(fmt, "csd-0", h264_csd0, sizeof(h264_csd0));
    AMediaFormat_setBuffer(fmt, "csd-1", h264_csd1, sizeof(h264_csd1));
    NH264_LOGI("H264 FORCED csd-0/csd-1 set: sps=%zu pps=%zu", sizeof(h264_csd0), sizeof(h264_csd1));

    media_status_t st = AMediaCodec_configure(codec, fmt, window, NULL, 0);
    AMediaFormat_delete(fmt);

    if (st != AMEDIA_OK) {
        NH264_LOGE("AMediaCodec_configure failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    st = AMediaCodec_start(codec);

    if (st != AMEDIA_OK) {
        NH264_LOGE("AMediaCodec_start failed: %d", (int)st);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        NH264_LOGE("socket failed errno=%d", errno);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_native_h264_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        NH264_LOGE("bind UDP port %d failed errno=%d", g_native_h264_port, errno);
        close(sock);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        nh264_detach_env(attached);
        g_native_h264_running = 0;
        return NULL;
    }

    NH264_LOGI("Native H264 RTP receiver started port=%d size=%dx%d fps=%d",
        g_native_h264_port, g_native_h264_width, g_native_h264_height, g_native_h264_fps);

    NativeH264Buffer au = {0};
    NativeH264Buffer fu = {0};
    NativeH264RtpState rtp = {0};
    rtp.wait_for_idr = 1;

    uint8_t packet[65536];
    int64_t pts_us = 0;

    while (!g_native_h264_stop) {
        ssize_t n = recv(sock, packet, sizeof(packet), 0);

        if (n > 0) {
            nh264_inc_rtp();

            int marker = 0;

            nh264_depay_rtp(packet, (int)n, &au, &fu, &rtp, &marker);

            if (marker) {
                if (rtp.fu_started || fu.size > 0) {
                    nh264_drop_current_au(&au, &fu, &rtp, "marker reached with incomplete FU-A");
                }

                if (au.size > 0 && !rtp.au_corrupt && !rtp.wait_for_idr) {
                    nh264_queue_access_unit(codec, &au, g_native_h264_fps, &pts_us);
                } else {
                    if (au.size > 0 || rtp.au_corrupt) {
                        nh264_inc_drop();
                    }
                }

                nh264_clear(&au);
                nh264_clear(&fu);
                rtp.fu_started = 0;
                rtp.au_corrupt = 0;
            }
        }

        nh264_drain_codec(codec);
    }

    nh264_drain_codec(codec);

    nh264_free(&au);
    nh264_free(&fu);

    close(sock);

    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    ANativeWindow_release(window);

    NH264_LOGI("Native H264 RTP receiver stopped.");

    nh264_detach_env(attached);
    g_native_h264_running = 0;
    return NULL;
}

__attribute__((visibility("default"))) int gst_quest_native_h264_start(void *surface_obj, int width, int height, int port, int fps)
{
    if (g_native_h264_running) {
        NH264_LOGI("Native H264 receiver already running.");
        return 1;
    }

    int attached = 0;
    JNIEnv *env = nh264_get_env(&attached);

    if (!env) {
        NH264_LOGE("native_h264_start failed: no JNI env.");
        return 0;
    }

    if (surface_obj == NULL) {
        NH264_LOGE("native_h264_start failed: surface is NULL.");
        nh264_detach_env(attached);
        return 0;
    }

    if (g_native_h264_surface_global != NULL) {
        (*env)->DeleteGlobalRef(env, g_native_h264_surface_global);
        g_native_h264_surface_global = NULL;
    }

    g_native_h264_surface_global = (*env)->NewGlobalRef(env, (jobject)surface_obj);

    g_native_h264_width = width;
    g_native_h264_height = height;
    g_native_h264_port = port;
    g_native_h264_fps = fps > 0 ? fps : 60;

    pthread_mutex_lock(&g_native_h264_stats_mutex);
    g_native_h264_rtp_packets = 0;
    g_native_h264_access_units = 0;
    g_native_h264_decoded_frames = 0;
    g_native_h264_dropped_packets = 0;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);

    g_native_h264_stop = 0;
    g_native_h264_running = 1;

    int rc = pthread_create(&g_native_h264_thread, NULL, nh264_thread_main, NULL);

    if (rc != 0) {
        g_native_h264_running = 0;
        NH264_LOGE("pthread_create failed rc=%d", rc);
        nh264_detach_env(attached);
        return 0;
    }

    nh264_detach_env(attached);
    return 1;
}

__attribute__((visibility("default"))) void gst_quest_native_h264_stop(void)
{
    if (!g_native_h264_running) return;

    g_native_h264_stop = 1;
    pthread_join(g_native_h264_thread, NULL);

    int attached = 0;
    JNIEnv *env = nh264_get_env(&attached);

    if (env && g_native_h264_surface_global != NULL) {
        (*env)->DeleteGlobalRef(env, g_native_h264_surface_global);
        g_native_h264_surface_global = NULL;
    }

    nh264_detach_env(attached);
}

__attribute__((visibility("default"))) int gst_quest_native_h264_get_running(void)
{
    return g_native_h264_running;
}

__attribute__((visibility("default"))) int gst_quest_native_h264_get_rtp_packets(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    unsigned long long v = g_native_h264_rtp_packets;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h264_get_access_units(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    unsigned long long v = g_native_h264_access_units;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h264_get_decoded_frames(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    unsigned long long v = g_native_h264_decoded_frames;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}

__attribute__((visibility("default"))) int gst_quest_native_h264_get_dropped_packets(void)
{
    pthread_mutex_lock(&g_native_h264_stats_mutex);
    unsigned long long v = g_native_h264_dropped_packets;
    pthread_mutex_unlock(&g_native_h264_stats_mutex);
    return v > 2147483647ULL ? 2147483647 : (int)v;
}



