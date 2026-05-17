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

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
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

    if (is_av1) {
        LOGI("Incoming stream detected as AV1. Building AV1 decode RGBA chain.");

        GstElement *queue = gst_element_factory_make("queue", "av1_in_queue");
        GstElement *depay = gst_element_factory_make("rtpav1depay", "av1_depay");
        GstElement *parse = gst_element_factory_make("av1parse", "av1_parse");
        const gchar *selected_decoder_factory = NULL;
        GstElement *decoder = create_av1_decoder_with_fallback(&selected_decoder_factory);
        GstElement *convert = gst_element_factory_make("videoconvert", "av1_videoconvert");
        GstElement *sink = gst_element_factory_make("appsink", "decoded_frame_sink_av1");

        if (queue == NULL || depay == NULL || parse == NULL || decoder == NULL || convert == NULL || sink == NULL) {
            LOGE("Could not create one or more AV1 decode elements");
            LOGE(
                "queue=%p depay=%p parse=%p decoder=%p selected_decoder=%s convert=%p appsink=%p",
                queue,
                depay,
                parse,
                decoder,
                selected_decoder_factory != NULL ? selected_decoder_factory : "NULL",
                convert,
                sink
            );

            if (queue != NULL) gst_object_unref(queue);
            if (depay != NULL) gst_object_unref(depay);
            if (parse != NULL) gst_object_unref(parse);
            if (decoder != NULL) gst_object_unref(decoder);
            if (convert != NULL) gst_object_unref(convert);
            if (sink != NULL) gst_object_unref(sink);

            return;
        }
        /* AV1 Phase 2 live stability:
         * keep the AV1 software path real-time. If RTP/depay/decode falls behind,
         * prefer dropping old data and requesting a clean keyframe over freezing
         * the Unity texture on an old frame.
         */
        g_object_set(queue,
            "max-size-buffers", 4,
            "max-size-bytes", 0,
            "max-size-time", 0,
            "leaky", 2,
            NULL);

        g_object_set(depay,
            "request-keyframe", TRUE,
            "wait-for-keyframe", TRUE,
            NULL);

        g_object_set(sink,
            "emit-signals", TRUE,
            "sync", FALSE,
            "max-buffers", 1,
            "drop", TRUE,
            "enable-last-sample", FALSE,
            NULL);



        g_object_set(
            queue,
            "max-size-buffers", 16,
            "max-size-bytes", 0,
            "max-size-time", 0,
            NULL
        );

        GstCaps *appsink_caps = gst_caps_from_string("video/x-raw,format=RGBA");

        g_object_set(
            sink,
            "caps", appsink_caps,
            "emit-signals", TRUE,
            "sync", FALSE,
            "max-buffers", 2,
            "drop", TRUE,
            NULL
        );

        if (appsink_caps != NULL) {
            gst_caps_unref(appsink_caps);
        }

        g_signal_connect(sink, "new-sample", G_CALLBACK(on_decoded_sample), session);

        gst_bin_add_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            decoder,
            convert,
            sink,
            NULL
        );

        if (!gst_element_link_many(queue, depay, parse, decoder, convert, sink, NULL)) {
            LOGE(
                "Could not link queue -> rtpav1depay -> av1parse -> decoder(%s) -> videoconvert -> appsink",
                selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
            );
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }
        LOGI("AV1 Phase 2: verbose RTP/depay/parse probes disabled for live playback.");
GstPad *queue_sink_pad = gst_element_get_static_pad(queue, "sink");

        if (queue_sink_pad == NULL) {
            LOGE("Could not get AV1 queue sink pad");
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }

        GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
        gst_object_unref(queue_sink_pad);

        if (ret != GST_PAD_LINK_OK) {
            LOGE("Could not link incoming WebRTC pad to AV1 queue. ret=%d", ret);
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }

        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(sink);

        LOGI(
            "Incoming WebRTC media linked to AV1 RGBA appsink pipeline using decoder: %s",
            selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
        );

        return;
    }

    if (is_h265) {
        LOGI("Incoming stream detected as H.265. Building H.265 decode RGBA chain.");

        GstElement *queue = gst_element_factory_make("queue", "h265_in_queue");
        GstElement *depay = gst_element_factory_make("rtph265depay", "h265_depay");
        GstElement *parse = gst_element_factory_make("h265parse", "h265_parse");
        const gchar *selected_decoder_factory = NULL;
        GstElement *decoder = create_h265_decoder_with_fallback(&selected_decoder_factory);
        GstElement *convert = gst_element_factory_make("videoconvert", "h265_videoconvert");
        GstElement *sink = gst_element_factory_make("appsink", "decoded_frame_sink_h265");

        if (queue == NULL || depay == NULL || parse == NULL || decoder == NULL || convert == NULL || sink == NULL) {
            LOGE("Could not create one or more H.265 decode elements");
            LOGE(
                "queue=%p depay=%p parse=%p decoder=%p selected_decoder=%s convert=%p appsink=%p",
                queue,
                depay,
                parse,
                decoder,
                selected_decoder_factory != NULL ? selected_decoder_factory : "NULL",
                convert,
                sink
            );

            if (queue != NULL) gst_object_unref(queue);
            if (depay != NULL) gst_object_unref(depay);
            if (parse != NULL) gst_object_unref(parse);
            if (decoder != NULL) gst_object_unref(decoder);
            if (convert != NULL) gst_object_unref(convert);
            if (sink != NULL) gst_object_unref(sink);

            return;
        }

        g_object_set(
            queue,
            "max-size-buffers", 16,
            "max-size-bytes", 0,
            "max-size-time", 0,
            NULL
        );

        GstCaps *appsink_caps = gst_caps_from_string("video/x-raw,format=RGBA");

        g_object_set(
            sink,
            "caps", appsink_caps,
            "emit-signals", TRUE,
            "sync", FALSE,
            "max-buffers", 2,
            "drop", TRUE,
            NULL
        );

        if (appsink_caps != NULL) {
            gst_caps_unref(appsink_caps);
        }

        g_signal_connect(sink, "new-sample", G_CALLBACK(on_decoded_sample), session);

        gst_bin_add_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            decoder,
            convert,
            sink,
            NULL
        );

        if (!gst_element_link_many(queue, depay, parse, decoder, convert, sink, NULL)) {
            LOGE(
                "Could not link queue -> rtph265depay -> h265parse -> decoder(%s) -> videoconvert -> appsink",
                selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
            );
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }

        add_buffer_probe(queue, "sink", "incoming WebRTC RTP -> H.265 queue sink");
        add_buffer_probe(queue, "src", "H.265 queue src -> rtph265depay");
        add_buffer_probe(depay, "src", "rtph265depay src");
        add_buffer_probe(parse, "src", "h265parse src");

        GstPad *queue_sink_pad = gst_element_get_static_pad(queue, "sink");

        if (queue_sink_pad == NULL) {
            LOGE("Could not get H.265 queue sink pad");
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }

        GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
        gst_object_unref(queue_sink_pad);

        if (ret != GST_PAD_LINK_OK) {
            LOGE("Could not link incoming WebRTC pad to H.265 queue. ret=%d", ret);
            gst_bin_remove_many(
                GST_BIN(session->pipeline),
                queue,
                depay,
                parse,
                decoder,
                convert,
                sink,
                NULL
            );
            return;
        }

        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(sink);

        LOGI(
            "Incoming WebRTC media linked to H.265 RGBA appsink pipeline using decoder: %s",
            selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
        );

        return;
    }

    LOGI("Incoming stream detected as H.264. Building H.264 decode RGBA chain.");

    GstElement *queue = gst_element_factory_make("queue", "h264_in_queue");
    GstElement *depay = gst_element_factory_make("rtph264depay", "h264_depay");
    GstElement *parse = gst_element_factory_make("h264parse", "h264_parse");
    const gchar *selected_decoder_factory = NULL;
    GstElement *decoder = create_h264_decoder_with_fallback(&selected_decoder_factory);
    GstElement *convert = gst_element_factory_make("videoconvert", "h264_videoconvert");
    GstElement *sink = gst_element_factory_make("appsink", "decoded_frame_sink_h264");

    if (queue == NULL || depay == NULL || parse == NULL || decoder == NULL || convert == NULL || sink == NULL) {
        LOGE("Could not create one or more H.264 decode elements");
        LOGE(
            "queue=%p depay=%p parse=%p decoder=%p selected_decoder=%s convert=%p appsink=%p",
            queue,
            depay,
            parse,
            decoder,
            selected_decoder_factory != NULL ? selected_decoder_factory : "NULL",
            convert,
            sink
        );

        if (queue != NULL) gst_object_unref(queue);
        if (depay != NULL) gst_object_unref(depay);
        if (parse != NULL) gst_object_unref(parse);
        if (decoder != NULL) gst_object_unref(decoder);
        if (convert != NULL) gst_object_unref(convert);
        if (sink != NULL) gst_object_unref(sink);

        return;
    }

    g_object_set(
        queue,
        "max-size-buffers", 16,
        "max-size-bytes", 0,
        "max-size-time", 0,
        NULL
    );

    GstCaps *appsink_caps = gst_caps_from_string("video/x-raw,format=RGBA");

    g_object_set(
        sink,
        "caps", appsink_caps,
        "emit-signals", TRUE,
        "sync", FALSE,
        "max-buffers", 2,
        "drop", TRUE,
        NULL
    );

    if (appsink_caps != NULL) {
        gst_caps_unref(appsink_caps);
    }

    g_signal_connect(sink, "new-sample", G_CALLBACK(on_decoded_sample), session);

    gst_bin_add_many(
        GST_BIN(session->pipeline),
        queue,
        depay,
        parse,
        decoder,
        convert,
        sink,
        NULL
    );

    if (!gst_element_link_many(queue, depay, parse, decoder, convert, sink, NULL)) {
        LOGE(
            "Could not link queue -> rtph264depay -> h264parse -> decoder(%s) -> videoconvert -> appsink",
            selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
        );
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            decoder,
            convert,
            sink,
            NULL
        );
        return;
    }


    add_buffer_probe(queue, "sink", "incoming WebRTC RTP -> H.264 queue sink");
    add_buffer_probe(queue, "src", "H.264 queue src -> rtph264depay");
    add_buffer_probe(depay, "src", "rtph264depay src");
    add_buffer_probe(parse, "src", "h264parse src");

    GstPad *queue_sink_pad = gst_element_get_static_pad(queue, "sink");

    if (queue_sink_pad == NULL) {
        LOGE("Could not get queue sink pad");
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            decoder,
            convert,
            sink,
            NULL
        );
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, queue_sink_pad);
    gst_object_unref(queue_sink_pad);

    if (ret != GST_PAD_LINK_OK) {
        LOGE("Could not link incoming WebRTC pad to queue. ret=%d", ret);
        gst_bin_remove_many(
            GST_BIN(session->pipeline),
            queue,
            depay,
            parse,
            decoder,
            convert,
            sink,
            NULL
        );
        return;
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(sink);

    LOGI(
        "Incoming WebRTC media linked to H.264 RGBA appsink pipeline using decoder: %s",
        selected_decoder_factory != NULL ? selected_decoder_factory : "NULL"
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

            LOGI("Signaling TCP connection closed; keeping WebRTC media pipeline alive.");
            set_last_message("Signaling closed; media pipeline kept alive");

            while (g_signaling_running) {
                poll_pipeline_bus(session);
                sleep(1);
            }

            webrtc_session_destroy(session);
            session = NULL;
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