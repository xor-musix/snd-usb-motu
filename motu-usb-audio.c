// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU Pro Audio USB Driver
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/once.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/initval.h>
#include <sound/pcm.h>

MODULE_DESCRIPTION("MOTU Pro Audio USB Driver");
MODULE_AUTHOR("Dylan Robinson <dylan_robinson@motu.com>");
MODULE_LICENSE("GPL v2");

/*
 * Module parameters
 *
 * Allow selecting the sample rate at module load time.
 * Supported rates: 44100, 48000, 88200, 96000, 176400, 192000.
 *
 * All parameters are read-only at runtime: the URB pool and buffer ring are
 * sized from them at probe time and the streaming code reads them directly.
 */
static int sample_rate = 48000;
module_param(sample_rate, int, 0444);
MODULE_PARM_DESC(sample_rate, "Sample rate in Hz (44100, 48000 (default), 88200, 96000, 176400, 192000)");

static int uframes_per_urb = 512;
module_param(uframes_per_urb, int, 0444);
MODULE_PARM_DESC(uframes_per_urb, "USB microframes per URB (32, 64, 128, 256, 512 (default))");

static int num_urbs = 4;
module_param(num_urbs, int, 0444);
MODULE_PARM_DESC(num_urbs, "Number of isochronous URBs per stream (4 (default))");

static int pb_safety_offset = 16;
module_param(pb_safety_offset, int, 0444);
MODULE_PARM_DESC(pb_safety_offset, "Playback startup safety offset in microframes (2, 4, 8, 16 (default), 32)");

static int rec_safety_offset = 16;
module_param(rec_safety_offset, int, 0444);
MODULE_PARM_DESC(rec_safety_offset, "Capture startup safety offset in microframes (2, 4, 8, 16 (default), 32)");

static int bpf_factor = 32;
module_param(bpf_factor, int, 0444);
MODULE_PARM_DESC(bpf_factor, "Minimum ALSA period size in bytes multiplicator (16, 32 (default))");

static int use_cfc = -1;
module_param(use_cfc, int, 0444);
MODULE_PARM_DESC(use_cfc, "Isochronous scheduling: -1 auto (default), 0 always URB_ISO_ASAP, 1 xHCI CFC frame scheduling if the controller supports it");

static bool auto_restart = true;
module_param(auto_restart, bool, 0444);
MODULE_PARM_DESC(auto_restart, "Restart the USB streams when a gap in the isochronous stream is detected (default: Y)");

static bool restart_xrun = true;
module_param(restart_xrun, bool, 0444);
MODULE_PARM_DESC(restart_xrun, "Report an xrun to ALSA when the streams are restarted after a gap (default: Y)");

static int index = SNDRV_DEFAULT_IDX1;
module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for the MOTU card (default: first free slot)");

static char *id = SNDRV_DEFAULT_STR1;
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for the MOTU card (default: motuusb)");

static bool test_hooks;
module_param(test_hooks, bool, 0444);
MODULE_PARM_DESC(test_hooks, "Create test hooks (proc file inject_gap) for exercising gap recovery; testing only (default: N)");

static int total_uframes;

#define NUM_INTERRUPT_URBS  4
#define NUM_CH              24
#define BYTES_PER_SAMPLE    3
#define BYTES_PER_FRAME     (NUM_CH * BYTES_PER_SAMPLE)

/*
 * The device does not detect underflows of its playback buffer: after a gap
 * in the isochronous stream the channel mapping can shift by eight channels,
 * also seconds later, and stays shifted until the stream is restarted.
 * Nothing reports such a gap, so the driver watches for the conditions that
 * cause one and restarts the streams itself.
 */
#define RESTART_WINDOW      (10 * HZ)
#define RESTART_BURST       5       /* gap restarts per window before giving up */
/* with auto_restart=0, reports this close together count as one gap */
#define GAP_MERGE           msecs_to_jiffies(200)

/*
 * xHCI frame numbers (MFINDEX >> 3) wrap at 2048. Queued data is assumed
 * to run out FRAME_SLACK frames after the arithmetic end of the queue: the
 * first URB of a stream starts up to IST (at most 8 frames) plus the xHCI
 * driver's scheduling delay after submission.
 */
#define XHCI_FRAME_MASK     0x7ff
#define FRAME_SLACK         10

/*
 * The driver does not set the device clock; it must already run at the
 * stream rate. Capture packets show the real rate: warn if it is off by
 * more than 1/RATE_TOLERANCE for RATE_MISMATCH_URBS URBs in a row.
 */
#define RATE_TOLERANCE      50
#define RATE_SKIP_URBS      2       /* stream startup is not representative */
#define RATE_MISMATCH_URBS  3

/* xHCI HCCPARAMS1: Contiguous Frame ID Capability */
#define XHCI_HCCPARAMS1     0x10
#define XHCI_HCC_CFC        BIT(11)

/*
 * First kernel whose xHCI driver schedules CFC streams without creating
 * gaps itself (kernel bugzilla 220748): add8469b3e00 "xhci: fix frame id
 * calculation and checks for isoc URBs", b08d153c95f1 "xhci: Set frame ID
 * field of isoc TRB when starting an isoch stream" and the regression fix
 * 05506a76f13a "usb: xhci: Fix isochronous scheduling regression", all in
 * v7.3 (the last one from v7.3-rc2 on). Older kernels can silently move
 * the start of a stream by a frame or restart it ASAP after the ring
 * drained, which the device cannot tolerate. use_cfc=1 bypasses this check.
 */
#define CFC_MIN_KERNEL      KERNEL_VERSION(7, 3, 0)

/* gap restarts per window in CFC mode before falling back to URB_ISO_ASAP */
#define CFC_FALLBACK_BURST  2

static inline unsigned int rate_flag(unsigned int sr)
{
    switch (sr) {
    case 44100:  return SNDRV_PCM_RATE_44100;
    case 48000:  return SNDRV_PCM_RATE_48000;
    case 88200:  return SNDRV_PCM_RATE_88200;
    case 96000:  return SNDRV_PCM_RATE_96000;
    case 176400: return SNDRV_PCM_RATE_176400;
    case 192000: return SNDRV_PCM_RATE_192000;
    default:     return 0;
    }
}

static inline int compute_nom_sample_count(unsigned int sr)
{
    switch (sr) {
    case 44100:
    case 48000:  return 6;
    case 88200:  return 11;
    case 96000:  return 12;
    case 176400: return 22;
    case 192000: return 24;
    default:     return -EINVAL;
    }
}

static const unsigned int supported_rates[] = {
    44100, 48000, 88200, 96000, 176400, 192000
};

/* Mask of all supported rates for initial hw template; narrowed at open() */
#define SUPPORTED_RATES_MASK ( \
    SNDRV_PCM_RATE_44100  | \
    SNDRV_PCM_RATE_48000  | \
    SNDRV_PCM_RATE_88200  | \
    SNDRV_PCM_RATE_96000  | \
    SNDRV_PCM_RATE_176400 | \
    SNDRV_PCM_RATE_192000   \
)

static bool is_valid_value(int val, const int *valid, int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (val == valid[i])
            return true;
    return false;
}

static void validate_module_params(struct device *dev)
{
    static const int valid_uframes[] = { 32, 64, 128, 256, 512 };
    /*
     * Playback keeps two URBs in flight and reuses the pool round-robin,
     * paced by capture completions: with fewer than four, the playback URB
     * due for resubmission has not completed yet.
     */
    static const int valid_num_urbs[] = { 4 };
    static const int valid_safety[]   = { 2, 4, 8, 16, 32 };
    static const int valid_bpf[]      = { 16, 32 };
    static const int valid_cfc[]      = { -1, 0, 1 };

    if (!is_valid_value(uframes_per_urb, valid_uframes, ARRAY_SIZE(valid_uframes))) {
        dev_warn(dev, "Invalid uframes_per_urb=%d, clamping to 512\n", uframes_per_urb);
        uframes_per_urb = 512;
    }
    if (!is_valid_value(num_urbs, valid_num_urbs, ARRAY_SIZE(valid_num_urbs))) {
        dev_warn(dev, "Invalid num_urbs=%d, clamping to 4\n", num_urbs);
        num_urbs = 4;
    }
    if (!is_valid_value(pb_safety_offset, valid_safety, ARRAY_SIZE(valid_safety))) {
        dev_warn(dev, "Invalid pb_safety_offset=%d, clamping to 16\n", pb_safety_offset);
        pb_safety_offset = 16;
    }
    if (!is_valid_value(rec_safety_offset, valid_safety, ARRAY_SIZE(valid_safety))) {
        dev_warn(dev, "Invalid rec_safety_offset=%d, clamping to 16\n", rec_safety_offset);
        rec_safety_offset = 16;
    }
    if (!is_valid_value(bpf_factor, valid_bpf, ARRAY_SIZE(valid_bpf))) {
        dev_warn(dev, "Invalid bpf_factor=%d, clamping to 32\n", bpf_factor);
        bpf_factor = 32;
    }
    if (!is_valid_value(use_cfc, valid_cfc, ARRAY_SIZE(valid_cfc))) {
        dev_warn(dev, "Invalid use_cfc=%d, using -1 (auto)\n", use_cfc);
        use_cfc = -1;
    }

    total_uframes = num_urbs * uframes_per_urb;

    dev_info(dev, "Module params: sample_rate=%d uframes_per_urb=%d num_urbs=%d "
             "pb_safety_offset=%d rec_safety_offset=%d bpf_factor=%d total_uframes=%d "
             "use_cfc=%d\n",
             sample_rate, uframes_per_urb, num_urbs,
             pb_safety_offset, rec_safety_offset, bpf_factor, total_uframes,
             use_cfc);
}

#define CIRC_SUB(a, b, size)    (((a) + (size) - (b)) % (size))

static struct usb_driver snd_usb_motu_driver;

enum pb_start_state {
    PB_START_IDLE = 0,
    PB_START_PRIME,
    PB_START_SYNC,
    PB_START_RUNNING
};

struct motu_buf
{
    unsigned int length;
    unsigned char *data;
};

struct motu_stream
{
    bool enabled;
    spinlock_t lock;
    struct snd_pcm_substream *substream;
    struct motu_usb_data *owner; /* back reference to parent */
    unsigned int period_frames;
    unsigned int period_count;
    unsigned int period_idx;
    unsigned int frame_pos;
    unsigned int hw_ptr;
    unsigned int max_packet_size;
    unsigned int copy_pos;
    unsigned int copy_frame;
    struct motu_buf *bufs;
    struct urb **urbs;
    unsigned long urbs_busy;    /* bit per URB: submitted, not yet completed */
    bool deadline_valid;
    unsigned int deadline;      /* frame number at which queued packets run out */
};

struct motu_interrupt
{
    unsigned int interval;
    struct urb *urbs[NUM_INTERRUPT_URBS];
    unsigned long urbs_idle;    /* bit per URB: resubmission failed */
};

struct motu_usb_data
{
    spinlock_t wq_lock;
    atomic_t streams_started;
    struct usb_device *usb;
    struct usb_interface *ctrl_intf;
    struct usb_interface *pb_intf;    /* claimed, NULL once released */
    struct usb_interface *rec_intf;
    struct snd_card *card;
    struct snd_pcm *pcm;
    bool suspended;               /* under wq_lock: no work may be queued */
    struct workqueue_struct *start_stop_wq;
    struct delayed_work start_streams_work;
    struct delayed_work stop_streams_work;
    struct work_struct restart_work;
    struct motu_interrupt interrupt;
    struct motu_stream rec_stream;
    struct motu_stream pb_stream;
    unsigned int urb_idx;
    unsigned int chase_pos;
    unsigned int rx_frames;
    int pb_adj;
    int pb_adj_total;
    enum pb_start_state pb_start_state;
    /* Runtime sample-rate state */
    struct mutex params_lock;     /* serialises hw_params of both directions */
    unsigned int cur_rate;        /* 0 until set; in Hz */
    int nom_sample_count;         /* derived from cur_rate */
    unsigned int rates;           /* SNDRV_PCM_RATE_* that fit the endpoints */
    /* Stream continuity monitoring */
    bool disconnected;
    bool frame_check;             /* frame number deadline check usable */
    bool use_cfc;                 /* URBs queued without URB_ISO_ASAP */
    atomic_t gap_pending;         /* gap reported for this stream run */
    unsigned long last_gap_time;  /* jiffies of the last gap counted */
    atomic_t gap_restart;         /* restart queued because of a gap */
    atomic_t inject_gap;          /* test hook: playback gap length in ms */
    bool inject_gap_running;
    unsigned long inject_gap_end;
    const char *last_gap;
    unsigned int gap_count;
    unsigned int restart_count;
    unsigned long restart_window;
    unsigned int restart_burst;
    /* Device clock monitoring */
    unsigned int rate_urbs;
    unsigned int rate_mismatches;
    unsigned int measured_rate;
};

struct motu_interrupt_msg
{
    unsigned char info;
    unsigned char attr;
    __le32 frame;
} __attribute__((packed));

/*
 * A microframe carries nom_sample_count +/- 1 sample frames. The capture
 * slots are pre-filled with nom_sample_count + 1 frames of shred pattern,
 * and playback packets mirror capture packets, so both endpoints must fit
 * that many frames per packet.
 */
static bool rate_fits_endpoints(const struct motu_usb_data *priv,
    unsigned int rate)
{
    int nsc = compute_nom_sample_count(rate);
    unsigned int bytes = (nsc + 1) * BYTES_PER_FRAME;

    return nsc > 0 && bytes <= priv->rec_stream.max_packet_size &&
        bytes <= priv->pb_stream.max_packet_size;
}

static int set_runtime_rate(struct motu_usb_data *priv, unsigned int rate)
{
    int nsc;

    if (!(priv->rates & rate_flag(rate)))
        return -EINVAL;

    nsc = compute_nom_sample_count(rate);
    if (nsc < 0)
        return -EINVAL;

    priv->cur_rate = rate;
    priv->nom_sample_count = nsc;
    dev_info(&priv->usb->dev, "Set runtime rate: %u Hz (nom_sample_count:%d)\n",
             rate, nsc);
    return 0;
}

struct class_interrupt_msg
{
    unsigned char info; /* Type */
    unsigned char attr; /* Attribute (Cur, Range, Mem) */
    unsigned char cn;   /* Control Number */
    unsigned char cs;   /* Control Selector */
    unsigned char intf; /* Interface */
    unsigned char id;   /* Entity ID */
} __attribute__((packed));

static struct snd_pcm_hardware snd_motu_hw = {
    .info = (SNDRV_PCM_INFO_MMAP |
             SNDRV_PCM_INFO_MMAP_VALID |
             SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_BLOCK_TRANSFER |
             SNDRV_PCM_INFO_BATCH),
    .formats =          SNDRV_PCM_FMTBIT_S24_3LE,
    .rates =            SUPPORTED_RATES_MASK,
    .rate_min =         44100,
    .rate_max =         192000,
    .channels_min =     NUM_CH,
    .channels_max =     NUM_CH,
    .buffer_bytes_max = BYTES_PER_FRAME * 2048 * 4,
    .period_bytes_min = BYTES_PER_FRAME * 32, /* updated at open() via bpf_factor */
    .period_bytes_max = BYTES_PER_FRAME * 2048,
    .periods_min =      2,
    .periods_max =      16,
};

static unsigned char motu_shred_pattern[BYTES_PER_FRAME];

static void reset_substream_position(struct motu_stream *stream)
{
    stream->period_idx = 0;
    stream->frame_pos = 0;
    stream->hw_ptr = 0;
}

static unsigned int substream_position(const struct motu_stream *stream)
{
    return (stream->period_idx * stream->period_frames) + stream->frame_pos;
}

static unsigned int substream_position_bytes(const struct motu_stream *stream)
{
    return substream_position(stream) * BYTES_PER_FRAME;
}

/**
 * inc_frame_position - Advance frame position and update period index
 * @stream: pointer to stream state
 * @count:  number of frames to increment
 *
 * Return: true if the period elapsed
 */
static bool inc_frame_position(struct motu_stream *stream, unsigned int count)
{
    stream->frame_pos += count;

    if (stream->frame_pos >= stream->period_frames) {
        stream->frame_pos -= stream->period_frames;
        
        if (++stream->period_idx >= stream->period_count)
            stream->period_idx = 0;
        
        stream->hw_ptr = stream->period_idx * stream->period_frames;
        
        return true;
    }

    return false;
}

/* Generate the shred pattern for a single frame. */
static void init_shred_pattern(void)
{
    for (int i = 0; i < BYTES_PER_FRAME; ++i)
        motu_shred_pattern[i] = ~(unsigned char)i;
}

/* Write the shred pattern to the audio buffer. */
static void shred_sample_frames(struct motu_stream *stream, unsigned char *buffer)
{
    unsigned char *ptr = buffer;

    for (int i = 0; i < (stream->owner->nom_sample_count + 1); ++i, ptr += BYTES_PER_FRAME)
        memcpy(ptr, motu_shred_pattern, BYTES_PER_FRAME);
}

/*
 * Count how many contiguous audio sample frames at the start of a USB
 * microframe buffer have been filled by the device (i.e. not equal to
 * the shred pattern).
 *
 * Return: number of contiguous filled sample frames
 */
static unsigned int count_uframe_sample_frames(struct motu_usb_data *priv, const unsigned char *buffer)
{
    const unsigned char *ptr = buffer;

    for (int i = 0; i < (priv->nom_sample_count + 1); ++i, ptr += BYTES_PER_FRAME)
        if (!memcmp(ptr, motu_shred_pattern, BYTES_PER_FRAME))
            return i;

    return (priv->nom_sample_count + 1);
}

/*
 * Count the total number of audio sample frames that have been filled by
 * the device across consecutive USB microframe buffers, starting from
 * priv->chase_pos.
 */
static void count_sample_frames(struct motu_usb_data *priv)
{
    struct motu_stream *rec_stream = &priv->rec_stream;
    unsigned int chase_pos = priv->chase_pos;
    unsigned int prev_frames = 0;
    unsigned int count = 0;

    for (int i = 0; i < uframes_per_urb; ++i) {
        struct motu_buf *buf = &rec_stream->bufs[chase_pos];

        buf->length = count_uframe_sample_frames(priv, buf->data);

        if (buf->length) {
            count += prev_frames;
            prev_frames = buf->length;
        }
        else if (count) {
            /* step back two to recheck last buffer next time */
            priv->chase_pos = CIRC_SUB(chase_pos, 2, total_uframes);
            break;
        }
        else if (i > 32) {
            /* too many buffers without finding any frames */
            break;
        }

        chase_pos = (chase_pos + 1) % total_uframes;
    }

    priv->rx_frames += count;
}

static void mute_period_to_usb(struct motu_stream *stream,
    unsigned int period_size)
{
    while (period_size) {
        struct motu_buf *dst_buf = &stream->bufs[stream->copy_pos];
        unsigned int dst_frames = dst_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(period_size, dst_frames);
        unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
        unsigned int dst_offset = stream->copy_frame * BYTES_PER_FRAME;

        memset(dst_buf->data + dst_offset, 0, bytes_this_copy);

        period_size -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= dst_buf->length) {
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) % total_uframes;
        }
    }
}

static void copy_period_to_usb(struct motu_stream *stream,
    unsigned int period_size)
{
    unsigned char* src;
    unsigned int src_offset;

    if (period_size != stream->period_frames) {
        mute_period_to_usb(stream, period_size);
        return;
    }

    src = stream->substream->runtime->dma_area;
    src_offset = substream_position_bytes(stream);

    while (period_size) {
        struct motu_buf *dst_buf = &stream->bufs[stream->copy_pos];
        unsigned int dst_frames = dst_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(period_size, dst_frames);
        unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
        unsigned int dst_offset = stream->copy_frame * BYTES_PER_FRAME;

        memcpy(dst_buf->data + dst_offset, src + src_offset, bytes_this_copy);

        src_offset += bytes_this_copy;
        period_size -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= dst_buf->length) {
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) % total_uframes;
        }
    }

    inc_frame_position(stream, stream->period_frames);
    snd_pcm_period_elapsed(stream->substream);
}

static void sync_period_from_usb(struct motu_stream *stream,
    unsigned int period_size)
{
    while (period_size) {
        struct motu_buf *src_buf = &stream->bufs[stream->copy_pos];
        unsigned int src_frames = src_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(period_size, src_frames);

        if (!frames_this_copy)
            break;

        period_size -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= src_buf->length) {
            shred_sample_frames(stream, stream->bufs[stream->copy_pos].data);
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) % total_uframes;
        }
    }
}

static void copy_period_from_usb(struct motu_stream *stream,
    unsigned int period_size)
{
    unsigned char* dst;
    unsigned int dst_offset;

    if (period_size != stream->period_frames) {
        sync_period_from_usb(stream, period_size);
        return;
    }

    dst = stream->substream->runtime->dma_area;
    dst_offset = substream_position_bytes(stream);

    while (period_size) {
        struct motu_buf *src_buf = &stream->bufs[stream->copy_pos];
        unsigned int src_frames = src_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(period_size, src_frames);
        unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
        unsigned int src_offset = stream->copy_frame * BYTES_PER_FRAME;

        if (!frames_this_copy)
            break;

        memcpy(dst + dst_offset, src_buf->data + src_offset, bytes_this_copy);

        dst_offset += bytes_this_copy;
        period_size -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= src_buf->length) {
            shred_sample_frames(stream, stream->bufs[stream->copy_pos].data);
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) % total_uframes;
        }
    }

    inc_frame_position(stream, stream->period_frames);
    snd_pcm_period_elapsed(stream->substream);
}

static void restart_streaming_endpoints(struct motu_usb_data *priv);

/* URB was unlinked or the device is gone: not a stream fault */
static bool urb_unlinked(int status)
{
    switch (status) {
    case -ENOENT:
    case -ECONNRESET:
    case -ESHUTDOWN:
    case -ENODEV:
    case -EPERM:        /* submitted while being killed */
        return true;
    default:
        return false;
    }
}

static unsigned int stream_urb_index(const struct motu_stream *stream,
    const struct urb *urb)
{
    unsigned int i;

    for (i = 0; i < num_urbs - 1; ++i)
        if (stream->urbs[i] == urb)
            break;

    return i;
}

/*
 * Report that the isochronous stream has, or is about to have, a gap. The
 * first report queues a restart; later ones are dropped until it has run.
 */
static void report_stream_gap(struct motu_usb_data *priv, const char *reason)
{
    if (!atomic_read(&priv->streams_started) || READ_ONCE(priv->disconnected))
        return;

    if (!auto_restart) {
        /*
         * Nothing repairs the stream, so every gap leaves the device in a
         * new state: count each one. One gap is usually seen by several
         * URB completions in a row.
         */
        if (atomic_xchg(&priv->gap_pending, 1) &&
            time_before(jiffies, READ_ONCE(priv->last_gap_time) + GAP_MERGE))
            return;
        WRITE_ONCE(priv->last_gap_time, jiffies);
        WRITE_ONCE(priv->last_gap, reason);
        WRITE_ONCE(priv->gap_count, priv->gap_count + 1);
        dev_warn_ratelimited(&priv->usb->dev,
            "Stream gap detected (%s), auto_restart disabled\n", reason);
        return;
    }

    if (atomic_cmpxchg(&priv->gap_pending, 0, 1))
        return;

    WRITE_ONCE(priv->last_gap, reason);
    WRITE_ONCE(priv->gap_count, priv->gap_count + 1);

    dev_warn_ratelimited(&priv->usb->dev,
        "Stream gap detected (%s), restarting streams\n", reason);
    atomic_set(&priv->gap_restart, 1);
    restart_streaming_endpoints(priv);
}

/*
 * Start tracking when the packets queued on @stream run out. Called right
 * after the first URB of a stream was submitted; the controller schedules
 * it at most a few frames later.
 */
static void set_stream_deadline(struct motu_usb_data *priv,
    struct motu_stream *stream, const struct urb *urb)
{
    int frame;

    if (!priv->frame_check)
        return;

    frame = usb_get_current_frame_number(priv->usb);
    if (frame < 0)
        return;

    stream->deadline = (frame + FRAME_SLACK + urb->number_of_packets / 8) &
        XHCI_FRAME_MASK;
    stream->deadline_valid = true;
}

/*
 * True if the frame counter has passed the end of the queued packets, i.e.
 * the controller has nothing left to send or receive. This also catches
 * the case where all queued URBs have completed in hardware but their
 * completions were delayed, which the busy bits cannot see.
 */
static bool stream_ran_dry(struct motu_usb_data *priv,
    const struct motu_stream *stream)
{
    int frame;

    if (!priv->frame_check || !stream->deadline_valid)
        return false;

    frame = usb_get_current_frame_number(priv->usb);
    if (frame < 0)
        return false;

    /* frame >= deadline on the wrapping 11-bit counter */
    return ((frame - stream->deadline) & XHCI_FRAME_MASK) <=
        XHCI_FRAME_MASK / 2;
}

/* Submit URB @idx of @stream and extend its queue deadline. */
static int submit_stream_urb(struct motu_usb_data *priv,
    struct motu_stream *stream, unsigned int idx)
{
    struct urb *urb = stream->urbs[idx];
    int err;

    if (test_and_set_bit(idx, &stream->urbs_busy)) {
        /* the slot this URB covers has not been played out yet */
        report_stream_gap(priv, "URB still in flight");
        return -EBUSY;
    }

    err = usb_submit_urb(urb, GFP_ATOMIC);

    if (err) {
        clear_bit(idx, &stream->urbs_busy);

        if (!urb_unlinked(err)) {
            dev_warn_ratelimited(&priv->usb->dev,
                "URB submission failed: %d\n", err);
            report_stream_gap(priv, "URB submission failed");
        }
        return err;
    }

    if (stream->deadline_valid)
        stream->deadline = (stream->deadline + urb->number_of_packets / 8) &
            XHCI_FRAME_MASK;

    return 0;
}

static void handle_interval_interrupt(struct motu_usb_data *priv)
{
    struct motu_stream *rec_stream = &priv->rec_stream;
    struct motu_stream *pb_stream = &priv->pb_stream;
    unsigned int period_size = priv->interrupt.interval;
    int pb_adj;

    if (!atomic_read(&priv->streams_started))
        return;

    count_sample_frames(priv);

    if (rec_stream->copy_pos == total_uframes) {
        /* Capture startup - Sync copy position */
        if (priv->rx_frames < (period_size + rec_safety_offset))
            return;

        int discard = priv->rx_frames - (period_size + rec_safety_offset);
        rec_stream->copy_pos = 0;

        while (discard > 0) {
            if (rec_stream->copy_pos == total_uframes - 1) {
                /* startup took longer than the ring: positions are stale */
                rec_stream->copy_pos = total_uframes;
                report_stream_gap(priv, "capture sync failed");
                return;
            }
            discard -= rec_stream->bufs[rec_stream->copy_pos].length;
            shred_sample_frames(rec_stream, rec_stream->bufs[rec_stream->copy_pos].data);
            ++rec_stream->copy_pos;
        }

        dev_info(&priv->usb->dev,
            "Rec Copy Sync: %d (chase: %u, count: %u)\n",
            rec_stream->copy_pos, priv->chase_pos, priv->rx_frames);
    }
    else if (priv->pb_start_state == PB_START_SYNC) {
        /* Playback startup - Sync copy position */
        int adjust = (priv->chase_pos + pb_safety_offset) -
            (uframes_per_urb * 2);

        pb_stream->copy_pos = (total_uframes + adjust) % total_uframes;
        priv->pb_start_state = PB_START_RUNNING;

        dev_info(&priv->usb->dev,
            "PB Copy Sync: %u (adjust: %d chase: %u)\n",
            pb_stream->copy_pos, adjust, priv->chase_pos);
    }
    else if ((pb_adj = xchg(&priv->pb_adj, 0))) {
        pb_stream->copy_pos =
            (pb_stream->copy_pos + pb_adj) % total_uframes;
        WRITE_ONCE(priv->pb_adj_total, priv->pb_adj_total + pb_adj);
    }

    /*
     * The PCM callbacks hold the stream locks with interrupts disabled and
     * the trigger callbacks do not take them, so waiting here cannot
     * deadlock, also not when snd_pcm_period_elapsed() stops the stream.
     */
    if (rec_stream->copy_pos != total_uframes) {
        spin_lock(&rec_stream->lock);
        if (READ_ONCE(rec_stream->enabled) && rec_stream->substream)
            copy_period_from_usb(rec_stream, period_size);
        else
            sync_period_from_usb(rec_stream, period_size);
        spin_unlock(&rec_stream->lock);
    }

    if (priv->pb_start_state == PB_START_RUNNING) {
        spin_lock(&pb_stream->lock);
        if (READ_ONCE(pb_stream->enabled) && pb_stream->substream)
            copy_period_to_usb(pb_stream, period_size);
        else
            mute_period_to_usb(pb_stream, period_size);
        spin_unlock(&pb_stream->lock);
    }
}

static void handle_status_interrupt(struct class_interrupt_msg *msg,
    struct motu_usb_data *priv)
{
    /*
    dev_info(&priv->usb->dev,
        "info:%u attr:%u cn:%u cs:%u intf:%u id:%u\n",
        msg->info, msg->attr, msg->cn, msg->cs, msg->intf, msg->id);
    */
}

static unsigned int interrupt_urb_index(const struct motu_usb_data *priv,
    const struct urb *urb)
{
    unsigned int i;

    for (i = 0; i < NUM_INTERRUPT_URBS - 1; ++i)
        if (priv->interrupt.urbs[i] == urb)
            break;

    return i;
}

static void interrupt_complete_urb(struct urb* urb)
{
    struct motu_usb_data *priv = urb->context;
    struct motu_interrupt_msg *msg = urb->transfer_buffer;
    int err;

    if (urb_unlinked(urb->status))
        return;

    if (urb->status == 0) {
        if (urb->actual_length >= 2 && msg->info == 0x01 && msg->attr == 0x01)
            handle_interval_interrupt(priv);
        else
            handle_status_interrupt(urb->transfer_buffer, priv);
    }
    else {
        dev_warn_ratelimited(&priv->usb->dev,
            "Interrupt URB status %d\n", urb->status);
    }

    err = usb_submit_urb(urb, GFP_ATOMIC);

    if (err) {
        /* revived by the next stream start */
        set_bit(interrupt_urb_index(priv, urb), &priv->interrupt.urbs_idle);

        if (!urb_unlinked(err)) {
            dev_warn_ratelimited(&priv->usb->dev,
                "Interrupt URB resubmission failed: %d\n", err);
            report_stream_gap(priv, "interrupt URB lost");
        }
    }
}

static void playback_complete_urb(struct urb *urb)
{
    struct motu_usb_data *priv = urb->context;
    struct motu_stream *pb_stream = &priv->pb_stream;
    unsigned int errors = 0;

    clear_bit(stream_urb_index(pb_stream, urb), &pb_stream->urbs_busy);

    if (!atomic_read(&priv->streams_started) || urb_unlinked(urb->status))
        return;

    if (urb->status) {
        report_stream_gap(priv, "playback URB failed");
        return;
    }

    /* any packet that did not go out is a hole in the device's stream */
    for (int i = 0; i < urb->number_of_packets; ++i)
        if (urb->iso_frame_desc[i].status)
            ++errors;

    if (errors) {
        dev_warn_ratelimited(&priv->usb->dev,
            "%u playback packet errors\n", errors);
        report_stream_gap(priv, "playback packets missed");
    }
}

/* Compare the rate the device delivers with the configured one. */
static void check_device_rate(struct motu_usb_data *priv,
    unsigned int frames, unsigned int packets)
{
    unsigned int expected = priv->cur_rate;
    unsigned int rate;

    if (priv->rate_urbs < RATE_SKIP_URBS) {
        ++priv->rate_urbs;
        return;
    }

    /* too many packet errors to tell */
    if (packets < uframes_per_urb / 2)
        return;

    /* one packet per microframe, 8000 microframes per second */
    rate = frames * 8000 / packets;
    WRITE_ONCE(priv->measured_rate, rate);

    if ((rate > expected ? rate - expected : expected - rate) *
        RATE_TOLERANCE <= expected) {
        if (priv->rate_mismatches >= RATE_MISMATCH_URBS)
            dev_info(&priv->usb->dev,
                "Device sample rate back at %u Hz\n", expected);
        priv->rate_mismatches = 0;
        return;
    }

    if (++priv->rate_mismatches == RATE_MISMATCH_URBS)
        dev_warn(&priv->usb->dev,
            "Device delivers %u Hz but the stream runs at %u Hz: "
            "check the device clock\n", rate, expected);
}

static void capture_complete_urb(struct urb *urb)
{
    struct motu_usb_data *priv = urb->context;
    struct motu_stream *rec_stream = &priv->rec_stream;
    struct motu_stream *pb_stream = &priv->pb_stream;
    unsigned int pb_max_frames = pb_stream->max_packet_size / BYTES_PER_FRAME;
    unsigned int pb_idx;
    unsigned int errors = 0;
    unsigned int rx_frames = 0;
    unsigned int rx_packets = 0;
    bool missed = false;
    bool rec_dry, pb_dry;
    bool pb_busy;
    int inject_ms;
    struct urb *pb_urb;
    struct motu_buf *pb_bufs;

    clear_bit(stream_urb_index(rec_stream, urb), &rec_stream->urbs_busy);

    if (!atomic_read(&priv->streams_started) || urb_unlinked(urb->status))
        return;

    if (urb->status) {
        report_stream_gap(priv, "capture URB failed");
        /* without a restart coming, keep the stream going */
        if (auto_restart)
            return;
    }

    /*
     * This URB has completed; if nothing else is queued (or the queued
     * packets have already been played out while this completion was
     * pending), the controller ran dry and the stream has a gap.
     */
    rec_dry = !READ_ONCE(rec_stream->urbs_busy);
    if (rec_dry)
        report_stream_gap(priv, "capture queue drained");
    else if ((rec_dry = stream_ran_dry(priv, rec_stream)))
        report_stream_gap(priv, "capture queue ran dry");
    
    pb_idx = priv->urb_idx;
    pb_urb = pb_stream->urbs[pb_idx];
    pb_bufs = &pb_stream->bufs[pb_idx * uframes_per_urb];
    priv->urb_idx = (priv->urb_idx + 1) % num_urbs;

    /* never rewrite the packet descriptors of a URB the controller owns */
    pb_busy = test_bit(pb_idx, &pb_stream->urbs_busy);
    
    for (int i = 0; i < uframes_per_urb; ++i) {
        /* Nominal Packet Size */
        unsigned int frames = priv->nom_sample_count;
        int status = urb->iso_frame_desc[i].status;
        
        if (!status) {
            frames = urb->iso_frame_desc[i].actual_length / BYTES_PER_FRAME;
            rx_frames += frames;
            ++rx_packets;
        }
        else {
            ++errors;
            /* missed service interval: the schedule slipped */
            if (status == -EXDEV)
                missed = true;
        }

        if (pb_busy)
            continue;

        frames = min(frames, pb_max_frames);
        
        pb_urb->iso_frame_desc[i].length = frames * BYTES_PER_FRAME;
        pb_bufs[i].length = frames;
    }

    if (errors)
        dev_warn_ratelimited(&priv->usb->dev,
            "%u capture packet errors\n", errors);

    if (missed)
        report_stream_gap(priv, "capture packets missed");

    check_device_rate(priv, rx_frames, rx_packets);

    /*
     * A dry queue starts again at the next possible frame, after the end of
     * the old schedule: count the deadline from there, or without a restart
     * every later completion reports the same gap again.
     */
    if (!submit_stream_urb(priv, rec_stream, stream_urb_index(rec_stream, urb)) &&
        rec_dry)
        set_stream_deadline(priv, rec_stream, urb);

    switch (priv->pb_start_state) {
    case PB_START_IDLE:
        priv->pb_start_state = PB_START_PRIME;
        break;
    case PB_START_PRIME:
        pb_stream->deadline_valid = false;
        if (!submit_stream_urb(priv, pb_stream, 0))
            set_stream_deadline(priv, pb_stream, pb_stream->urbs[0]);
        submit_stream_urb(priv, pb_stream, 1);
        priv->pb_start_state = PB_START_SYNC;
        break;
    case PB_START_SYNC:
    case PB_START_RUNNING:
        /*
         * Test hook: withhold playback URBs until the controller has none
         * left and the requested time has passed, then carry on. The
         * device sees a real gap in its stream.
         */
        if ((inject_ms = atomic_read(&priv->inject_gap))) {
            if (READ_ONCE(pb_stream->urbs_busy))
                break;
            if (!priv->inject_gap_running) {
                priv->inject_gap_running = true;
                priv->inject_gap_end = jiffies + msecs_to_jiffies(inject_ms);
            }
            if (time_before(jiffies, priv->inject_gap_end))
                break;
            priv->inject_gap_running = false;
            atomic_set(&priv->inject_gap, 0);
            dev_warn(&priv->usb->dev,
                "Test hook: playback gap of at least %d ms injected\n",
                inject_ms);
        }

        pb_dry = !READ_ONCE(pb_stream->urbs_busy);
        if (pb_dry)
            report_stream_gap(priv, "playback queue drained");
        else if ((pb_dry = stream_ran_dry(priv, pb_stream)))
            report_stream_gap(priv, "playback queue ran dry");

        if (!submit_stream_urb(priv, pb_stream, pb_idx) && pb_dry)
            set_stream_deadline(priv, pb_stream, pb_urb);
        break;
    }
}

static void set_interrupt_interval(struct motu_usb_data *priv, u16 interval)
{
    int ret;
    u8 request = 0x01;
    u8 request_type = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    u16 index = 0;
    int timeout = 500;
    
    priv->interrupt.interval = interval;
    ret = usb_control_msg(priv->usb, usb_sndctrlpipe(priv->usb, 0), request,
        request_type, interval, index, NULL, 0, timeout);

    if (ret < 0 && ret != -ENODEV)
        dev_warn(&priv->usb->dev,
            "Failed to set interrupt interval: %d\n", ret);
}

/*
 * Queue a full stream restart. Safe from any context, including URB
 * completions: the restart itself runs on the ordered start/stop workqueue.
 */
static void restart_streaming_endpoints(struct motu_usb_data *priv)
{
    unsigned long flags;

    spin_lock_irqsave(&priv->wq_lock, flags);

    if (priv->start_stop_wq && !priv->suspended)
        queue_work(priv->start_stop_wq, &priv->restart_work);

    spin_unlock_irqrestore(&priv->wq_lock, flags);
}

/* Resubmit interrupt URBs whose resubmission failed in their completion. */
static void revive_interrupt_urbs(struct motu_usb_data *priv)
{
    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i) {
        int err;

        if (!test_and_clear_bit(i, &priv->interrupt.urbs_idle))
            continue;

        err = usb_submit_urb(priv->interrupt.urbs[i], GFP_KERNEL);
        if (err) {
            set_bit(i, &priv->interrupt.urbs_idle);
            dev_warn(&priv->usb->dev,
                "Interrupt URB %d resubmission failed: %d\n", i, err);
        }
    }
}

static unsigned int stream_interval(const struct motu_usb_data *priv)
{
    return max(priv->rec_stream.period_frames,
               priv->pb_stream.period_frames);
}

/* Let applications see that the audio was interrupted. */
static void stop_xrun_substreams(struct motu_usb_data *priv)
{
    struct motu_stream *streams[] = { &priv->rec_stream, &priv->pb_stream };
    unsigned long flags;

    for (int i = 0; i < ARRAY_SIZE(streams); ++i) {
        spin_lock_irqsave(&streams[i]->lock, flags);
        if (streams[i]->substream)
            snd_pcm_stop_xrun(streams[i]->substream);
        spin_unlock_irqrestore(&streams[i]->lock, flags);
    }
}

/* Return: false if the streams were already running */
static bool start_streams(struct motu_usb_data *priv)
{
    unsigned int interval = stream_interval(priv);
    struct urb **urbs;
    int err;

    if (atomic_cmpxchg(&priv->streams_started, 0, 1))
        return false;

    atomic_set(&priv->gap_pending, 0);
    atomic_set(&priv->inject_gap, 0);
    priv->inject_gap_running = false;
    priv->rate_urbs = 0;
    priv->rate_mismatches = 0;
    WRITE_ONCE(priv->measured_rate, 0);

    dev_info(&priv->usb->dev, "start_streaming_endpoints\n");
    
    urbs = priv->rec_stream.urbs;

    priv->rec_stream.copy_pos = total_uframes;
    priv->rec_stream.copy_frame = 0;
    priv->pb_stream.copy_pos = total_uframes;
    priv->pb_stream.copy_frame = 0;
    priv->urb_idx = 0;
    priv->chase_pos = 0;
    priv->rx_frames = 0;
    priv->pb_start_state = PB_START_IDLE;

    for (int i = 0; i < total_uframes; ++i) {
        priv->rec_stream.bufs[i].length = 0;
        priv->pb_stream.bufs[i].length = 0;
    }

    for (int i = 0; i < uframes_per_urb; ++i)
        shred_sample_frames(&priv->rec_stream, priv->rec_stream.bufs[i].data);

    priv->rec_stream.urbs_busy = 0;
    priv->rec_stream.deadline_valid = false;
    priv->pb_stream.urbs_busy = 0;
    priv->pb_stream.deadline_valid = false;

    revive_interrupt_urbs(priv);

    set_interrupt_interval(priv, interval);
    err = usb_set_interface(priv->usb, 2, 1); // start record
    if (!err)
        err = usb_set_interface(priv->usb, 1, 1); // start playback

    if (err) {
        dev_err(&priv->usb->dev,
            "Failed to select the streaming alternate settings: %d\n", err);
        set_interrupt_interval(priv, 0);
        usb_set_interface(priv->usb, 2, 0);
        atomic_set(&priv->streams_started, 0);
        /* let the application retry instead of waiting for periods */
        stop_xrun_substreams(priv);
        return true;
    }

    /*
     * Queue the whole capture pool back to back: being preempted between
     * submissions could let the first URB play out before the next one is
     * queued. Completions cannot run in between either.
     */
    local_bh_disable();
    if (!submit_stream_urb(priv, &priv->rec_stream, 0))
        set_stream_deadline(priv, &priv->rec_stream, urbs[0]);
    for (int i = 1; i < num_urbs; ++i)
        submit_stream_urb(priv, &priv->rec_stream, i);
    local_bh_enable();

    return true;
}

/* Return: false if the streams were not running */
static bool stop_streams(struct motu_usb_data *priv)
{
    int err;

    if (!atomic_cmpxchg(&priv->streams_started, 1, 0))
       return false;

    dev_info(&priv->usb->dev, "stop_streaming_endpoints\n");

    WRITE_ONCE(priv->pb_adj_total, 0);

    set_interrupt_interval(priv, 0);

    for (int i = 0; i < num_urbs; ++i) {
        usb_kill_urb(priv->rec_stream.urbs[i]);
        usb_kill_urb(priv->pb_stream.urbs[i]);
    }
    
    err = usb_set_interface(priv->usb, 2, 0); // stop record
    if (!err)
        err = usb_set_interface(priv->usb, 1, 0); // stop playback

    if (err && err != -ENODEV)
        dev_warn(&priv->usb->dev,
            "Failed to select alternate setting 0: %d\n", err);

    return true;
}

static void start_streaming_endpoints(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct motu_usb_data *priv =
        container_of(dwork, struct motu_usb_data, start_streams_work);

    if (!start_streams(priv) &&
        stream_interval(priv) != priv->interrupt.interval)
        restart_streaming_endpoints(priv);
}

static void stop_streaming_endpoints(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct motu_usb_data *priv =
        container_of(dwork, struct motu_usb_data, stop_streams_work);

    stop_streams(priv);
}

/*
 * Leave CFC frame scheduling for URB_ISO_ASAP. Only call while the streams
 * are stopped, as it rewrites the flags of idle URBs.
 */
static void fall_back_to_iso_asap(struct motu_usb_data *priv)
{
    struct motu_stream *streams[] = { &priv->rec_stream, &priv->pb_stream };

    dev_warn(&priv->usb->dev,
        "Repeated stream gaps with CFC scheduling, falling back to URB_ISO_ASAP\n");

    for (int i = 0; i < ARRAY_SIZE(streams); ++i)
        for (int j = 0; j < num_urbs; ++j)
            streams[i]->urbs[j]->transfer_flags |= URB_ISO_ASAP;

    priv->use_cfc = false;
}

/* Rate-limit gap restarts so a persistently broken stream cannot loop. */
static bool gap_restart_allowed(struct motu_usb_data *priv)
{
    if (time_after(jiffies, priv->restart_window + RESTART_WINDOW)) {
        priv->restart_window = jiffies;
        priv->restart_burst = 0;
    }

    return ++priv->restart_burst <= RESTART_BURST;
}

/*
 * The device does not recover from gaps in its playback stream on its own;
 * it needs: kill URBs, alternate setting 0, a few ms pause, alternate
 * setting 1, resubmit. Do the whole sequence in one work item so that it
 * cannot interleave with a pending delayed start or stop.
 */
static void restart_streaming_work(struct work_struct *work)
{
    struct motu_usb_data *priv =
        container_of(work, struct motu_usb_data, restart_work);
    /* a soft_reset or rate change after an unhandled gap is not a gap restart */
    bool gap = atomic_xchg(&priv->gap_restart, 0);

    if (gap && restart_xrun)
        stop_xrun_substreams(priv);

    if (!stop_streams(priv))
        return;

    if (gap && !gap_restart_allowed(priv)) {
        dev_err(&priv->usb->dev,
            "%u stream gaps within %u s, leaving streams stopped\n",
            priv->restart_burst, RESTART_WINDOW / HZ);
        if (!restart_xrun)
            stop_xrun_substreams(priv);
        return;
    }

    if (gap && priv->use_cfc && priv->restart_burst >= CFC_FALLBACK_BURST)
        fall_back_to_iso_asap(priv);

    usleep_range(5000, 10000);
    start_streams(priv);

    if (gap)
        WRITE_ONCE(priv->restart_count, priv->restart_count + 1);
}

static void free_stream_urbs(struct motu_usb_data *priv,
    struct motu_stream *stream)
{
    if (stream->urbs) {
        for (int i = 0; i < num_urbs; ++i) {
            if (!stream->urbs[i])
                continue;

            usb_kill_urb(stream->urbs[i]);

            if ((i == 0) && stream->urbs[i]->transfer_buffer)
            {
                usb_free_coherent(priv->usb,
                    stream->urbs[i]->transfer_buffer_length,
                    stream->urbs[i]->transfer_buffer,
                    stream->urbs[i]->transfer_dma);
            }

            usb_free_urb(stream->urbs[i]);
            stream->urbs[i] = NULL;
        }
        kfree(stream->urbs);
        stream->urbs = NULL;
    }
    kfree(stream->bufs);
    stream->bufs = NULL;
}

/*
 * All URBs of a stream share one transfer buffer of uframes_per_urb packet
 * slots: URB n+1 reuses the memory of URB n one URB duration later. The
 * bufs[] ring has total_uframes entries but aliases that buffer, which the
 * shred-pattern frame counting relies on. The effective audio ring is thus
 * one URB long; the other URBs only add scheduling headroom.
 */
static int alloc_stream_urbs(struct motu_usb_data *priv,
            struct motu_stream *stream,
            const struct usb_endpoint_descriptor *epd,
            usb_complete_t completion_handler)
{
    struct urb **urbs;
    unsigned int pipe;
    unsigned int max_packet_size;
    unsigned int transfer_buffer_length;
    unsigned char *transfer_buffer = NULL;
    dma_addr_t transfer_dma;
    
    if (usb_endpoint_dir_in(epd))
        pipe = usb_rcvisocpipe(priv->usb, usb_endpoint_num(epd));
    else
        pipe = usb_sndisocpipe(priv->usb, usb_endpoint_num(epd));
    
    max_packet_size = usb_endpoint_maxp(epd) * usb_endpoint_maxp_mult(epd);
    transfer_buffer_length = max_packet_size * uframes_per_urb;

    stream->bufs = kcalloc(total_uframes, sizeof(*stream->bufs), GFP_KERNEL);
    if (!stream->bufs)
        return -ENOMEM;

    stream->urbs = kcalloc(num_urbs, sizeof(*stream->urbs), GFP_KERNEL);
    if (!stream->urbs)
        goto out_free;

    urbs = stream->urbs;

    for (int i = 0; i < num_urbs; ++i) {
        unsigned int buf_base;

        urbs[i] = usb_alloc_urb(uframes_per_urb, GFP_KERNEL);
        
        if (!urbs[i])
            goto out_free;

        if (!transfer_buffer) {
            transfer_buffer = usb_alloc_coherent(priv->usb,
                                transfer_buffer_length,
                                GFP_KERNEL, &transfer_dma);

            if (!transfer_buffer)
                goto out_free;
        }
        
        urbs[i]->dev = priv->usb;
        urbs[i]->pipe = pipe;
        urbs[i]->transfer_flags = URB_NO_TRANSFER_DMA_MAP |
            (priv->use_cfc ? 0 : URB_ISO_ASAP);
        urbs[i]->transfer_buffer = transfer_buffer;
        urbs[i]->transfer_dma = transfer_dma;
        urbs[i]->transfer_buffer_length = max_packet_size * uframes_per_urb;
        urbs[i]->number_of_packets = uframes_per_urb;
        urbs[i]->interval = 1;
        urbs[i]->context = priv;
        urbs[i]->complete = completion_handler;

        buf_base = i * uframes_per_urb;

        for (int f = 0; f < uframes_per_urb; ++f) {
            unsigned int offset = max_packet_size * f;

            urbs[i]->iso_frame_desc[f].offset = offset;
            urbs[i]->iso_frame_desc[f].length = max_packet_size;

            stream->bufs[buf_base + f].data = transfer_buffer + offset;
        }
    }

    stream->max_packet_size = max_packet_size;

    return 0;

out_free:
    free_stream_urbs(priv, stream);
    return -ENOMEM;
}

static void free_interrupt_ubs(struct motu_usb_data *priv)
{
    struct urb **urbs = priv->interrupt.urbs;

    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i) {
        
        if (!urbs[i])
            continue;
        
        usb_kill_urb(urbs[i]);
        
        if (urbs[i]->transfer_buffer)
        {
            usb_free_coherent(priv->usb,
                urbs[i]->transfer_buffer_length,
                urbs[i]->transfer_buffer,
                urbs[i]->transfer_dma);
        }
        
        usb_free_urb(urbs[i]);

        urbs[i] = NULL;
    }
}

static int alloc_interrupt_urbs(struct motu_usb_data *priv,
            const struct usb_endpoint_descriptor *epd,
            usb_complete_t completion_handler)
{
    struct urb **urbs = priv->interrupt.urbs;
    unsigned int pipe = usb_rcvintpipe(priv->usb, usb_endpoint_num(epd));
    /* a buffer shorter than wMaxPacketSize turns longer messages into babble */
    unsigned int length = max_t(unsigned int, usb_endpoint_maxp(epd),
        sizeof(struct class_interrupt_msg));

    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i) {
        urbs[i] = usb_alloc_urb(0, GFP_KERNEL);

        if (!urbs[i])
            goto out_free;

        urbs[i]->dev = priv->usb;
        urbs[i]->pipe = pipe;
        urbs[i]->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
        urbs[i]->transfer_buffer = usb_alloc_coherent(priv->usb, length,
                                        GFP_KERNEL, &urbs[i]->transfer_dma);
        
        if (!urbs[i]->transfer_buffer)
            goto out_free;
        
        urbs[i]->transfer_buffer_length = length;
        urbs[i]->complete = completion_handler;
        urbs[i]->context = priv;
        urbs[i]->start_frame = -1;
        urbs[i]->interval = 1;
    }

    return 0;

out_free:
    free_interrupt_ubs(priv);
    return -ENOMEM;
}

static void start_interrupt_urbs(struct motu_usb_data *priv)
{
    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i) {
        int err = usb_submit_urb(priv->interrupt.urbs[i], GFP_KERNEL);

        if (err) {
            set_bit(i, &priv->interrupt.urbs_idle);
            dev_warn(&priv->usb->dev,
                "Interrupt URB %d submission failed: %d\n", i, err);
        }
    }
}

static void queue_start_streaming(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned long flags;

    spin_lock_irqsave(&priv->wq_lock, flags);
    cancel_delayed_work(&priv->stop_streams_work);
    if (priv->start_stop_wq && !priv->suspended)
        queue_delayed_work(priv->start_stop_wq, &priv->start_streams_work, 0);
    spin_unlock_irqrestore(&priv->wq_lock, flags);
}

static void maybe_queue_stop_streaming(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned long flags;

    /*
     * May run from snd_pcm_period_elapsed() in URB completion context.
     * Check under wq_lock: the other direction's start trigger sets its
     * enabled flag before taking the lock, so either this sees it or the
     * start is queued after this stop.
     */
    spin_lock_irqsave(&priv->wq_lock, flags);

    if (READ_ONCE(priv->rec_stream.enabled) || READ_ONCE(priv->pb_stream.enabled)) {
        spin_unlock_irqrestore(&priv->wq_lock, flags);
        return;
    }

    cancel_delayed_work(&priv->start_streams_work);
    if (priv->start_stop_wq && !priv->suspended)
        queue_delayed_work(priv->start_stop_wq, &priv->stop_streams_work,
            msecs_to_jiffies(2000));
    spin_unlock_irqrestore(&priv->wq_lock, flags);
}

/*
 * Capture and playback share one USB clock and one period interrupt, so
 * once one direction is configured the other must use the same rate and
 * period size. These rules are evaluated on every hw_refine and thus see
 * the other direction's current configuration.
 */
static int hw_rule_fixed(struct snd_pcm_hw_params *params,
    struct snd_pcm_hw_rule *rule, unsigned int value)
{
    struct snd_interval t = { .min = value, .max = value, .integer = 1 };

    if (!value)
        return 0;

    return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

static int hw_rule_rate(struct snd_pcm_hw_params *params,
    struct snd_pcm_hw_rule *rule)
{
    const struct motu_stream *other = rule->private;

    if (!READ_ONCE(other->period_frames))
        return 0;

    return hw_rule_fixed(params, rule, READ_ONCE(other->owner->cur_rate));
}

static int hw_rule_period_size(struct snd_pcm_hw_params *params,
    struct snd_pcm_hw_rule *rule)
{
    const struct motu_stream *other = rule->private;

    return hw_rule_fixed(params, rule, READ_ONCE(other->period_frames));
}

static int open_pcm_stream(struct snd_pcm_substream *subs,
    struct motu_stream *stream, struct motu_stream *other)
{
    struct motu_usb_data *priv = subs->private_data;
    struct snd_pcm_runtime *runtime = subs->runtime;
    int err;

    /*
     * The first playback period only elapses after two capture URBs have
     * completed (2 * uframes_per_urb / 8 ms, 128 ms by default) plus the
     * sync interrupt, and a restart after a gap adds to that. ALSA's
     * default read/write timeout is max(100 ms, 1.1 * buffer time), so a
     * small buffer failed with -EIO before the stream was running.
     */
    subs->wait_time = max(1000, 4 * uframes_per_urb / 8);

    runtime->hw = snd_motu_hw;
    runtime->hw.period_bytes_min = BYTES_PER_FRAME * bpf_factor;
    runtime->hw.rates = priv->rates;
    snd_pcm_limit_hw_rates(runtime);

    /* the period ring arithmetic assumes buffer = periods * period size */
    err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0)
        return err;

    err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
        hw_rule_rate, other, SNDRV_PCM_HW_PARAM_RATE, -1);
    if (err < 0)
        return err;

    err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
        hw_rule_period_size, other, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
    if (err < 0)
        return err;

    spin_lock_irq(&stream->lock);
    stream->substream = subs;
    stream->period_frames = 0;
    spin_unlock_irq(&stream->lock);

    return 0;
}

/*
 * Common part of hw_params; switches the device rate if nothing else uses
 * it. Both directions may be configured concurrently by different
 * applications, so check and commit under params_lock.
 */
static int set_pcm_stream_params(struct motu_usb_data *priv,
    struct motu_stream *stream, const struct motu_stream *other,
    struct snd_pcm_hw_params *hw_params)
{
    unsigned int rate = params_rate(hw_params);
    unsigned int period_frames = params_period_size(hw_params);
    int err = 0;

    if (!(priv->rates & rate_flag(rate)))
        return -EINVAL;

    mutex_lock(&priv->params_lock);

    if (other->period_frames) {
        if (other->period_frames != period_frames) {
            dev_info(&priv->usb->dev,
                "Capture and Playback must use same period size!\n");
            err = -EINVAL;
            goto unlock;
        }
        if (priv->cur_rate != rate) {
            dev_info(&priv->usb->dev,
                "Capture and Playback must use same sample rate!\n");
            err = -EINVAL;
            goto unlock;
        }
    }

    if (priv->cur_rate != rate) {
        bool running = atomic_read(&priv->streams_started);
        err = set_runtime_rate(priv, rate);
        if (err)
            goto unlock;
        if (running)
            restart_streaming_endpoints(priv);
    }

    spin_lock_irq(&stream->lock);
    reset_substream_position(stream);
    stream->period_count = params_periods(hw_params);
    stream->period_frames = period_frames;
    spin_unlock_irq(&stream->lock);

unlock:
    mutex_unlock(&priv->params_lock);
    return err;
}

static int capture_pcm_open(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "capture_pcm_open\n");

    return open_pcm_stream(subs, &priv->rec_stream, &priv->pb_stream);
}

static int capture_pcm_close(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    spin_lock_irq(&priv->rec_stream.lock);
    priv->rec_stream.substream = NULL;
    priv->rec_stream.period_frames = 0;
    spin_unlock_irq(&priv->rec_stream.lock);

    return 0;
}

static int capture_pcm_hw_params(struct snd_pcm_substream *subs,
            struct snd_pcm_hw_params *hw_params)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned int rate = params_rate(hw_params);

    dev_info(&priv->usb->dev, "Capture Channels: %u\n",
        params_channels(hw_params));
    dev_info(&priv->usb->dev, "Capture Sample Rate: %u\n",
        rate);
    dev_info(&priv->usb->dev, "Capture Buffer Bytes: %u\n",
        params_buffer_bytes(hw_params));
    dev_info(&priv->usb->dev, "Capture Periods: %u\n",
        params_periods(hw_params));
    dev_info(&priv->usb->dev, "Capture Period Size: %u\n",
        params_period_size(hw_params));

    return set_pcm_stream_params(priv, &priv->rec_stream,
        &priv->pb_stream, hw_params);
}

static int capture_pcm_hw_free(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "capture_pcm_hw_free\n");

    spin_lock_irq(&priv->rec_stream.lock);
    priv->rec_stream.period_frames = 0;
    spin_unlock_irq(&priv->rec_stream.lock);

    WRITE_ONCE(priv->rec_stream.enabled, false);
    maybe_queue_stop_streaming(subs);

    return 0;
}

static int capture_pcm_prepare(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "capture_pcm_prepare\n");

    spin_lock_irq(&priv->rec_stream.lock);
    reset_substream_position(&priv->rec_stream);
    spin_unlock_irq(&priv->rec_stream.lock);

    return 0;
}

static int capture_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
    struct motu_usb_data *priv = subs->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        dev_info(&priv->usb->dev,
            "capture_pcm_trigger start (stop_threshold:%lu)\n",
            subs->runtime->stop_threshold);
        WRITE_ONCE(priv->rec_stream.enabled, true);
        queue_start_streaming(subs);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
        dev_info(&priv->usb->dev, "capture_pcm_trigger stop\n");
        WRITE_ONCE(priv->rec_stream.enabled, false);
        maybe_queue_stop_streaming(subs);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static snd_pcm_uframes_t capture_pcm_pointer(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    return priv->rec_stream.hw_ptr;
}

static const struct snd_pcm_ops capture_pcm_ops = {
    .open = capture_pcm_open,
    .close = capture_pcm_close,
    .hw_params = capture_pcm_hw_params,
    .hw_free = capture_pcm_hw_free,
    .prepare = capture_pcm_prepare,
    .trigger = capture_pcm_trigger,
    .pointer = capture_pcm_pointer,
};

static int playback_pcm_open(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_open\n");

    return open_pcm_stream(subs, &priv->pb_stream, &priv->rec_stream);
}

static int playback_pcm_close(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_close\n");

    spin_lock_irq(&priv->pb_stream.lock);
    priv->pb_stream.substream = NULL;
    priv->pb_stream.period_frames = 0;
    spin_unlock_irq(&priv->pb_stream.lock);

    return 0;
}

static int playback_pcm_hw_params(struct snd_pcm_substream *subs,
            struct snd_pcm_hw_params *hw_params)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned int rate = params_rate(hw_params);

    dev_info(&priv->usb->dev, "Playback Channels: %u\n",
        params_channels(hw_params));
    dev_info(&priv->usb->dev, "Playback Sample Rate: %u\n",
        rate);
    dev_info(&priv->usb->dev, "Playback Buffer Bytes: %u\n",
        params_buffer_bytes(hw_params));
    dev_info(&priv->usb->dev, "Playback Periods: %u\n",
        params_periods(hw_params));
    dev_info(&priv->usb->dev, "Playback Period Size: %u\n",
        params_period_size(hw_params));

    return set_pcm_stream_params(priv, &priv->pb_stream,
        &priv->rec_stream, hw_params);
}

static int playback_pcm_hw_free(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_hw_free\n");

    spin_lock_irq(&priv->pb_stream.lock);
    priv->pb_stream.period_frames = 0;
    spin_unlock_irq(&priv->pb_stream.lock);

    WRITE_ONCE(priv->pb_stream.enabled, false);
    maybe_queue_stop_streaming(subs);

    return 0;
}

static int playback_pcm_prepare(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_prepare\n");

    spin_lock_irq(&priv->pb_stream.lock);
    reset_substream_position(&priv->pb_stream);
    spin_unlock_irq(&priv->pb_stream.lock);

    return 0;
}

static int playback_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
    struct motu_usb_data *priv = subs->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        dev_info(&priv->usb->dev,
            "playback_pcm_trigger start (stop_threshold:%lu)\n",
            subs->runtime->stop_threshold);
        WRITE_ONCE(priv->pb_stream.enabled, true);
        queue_start_streaming(subs);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
        dev_info(&priv->usb->dev, "playback_pcm_trigger stop\n");
        WRITE_ONCE(priv->pb_stream.enabled, false);
        maybe_queue_stop_streaming(subs);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static snd_pcm_uframes_t playback_pcm_pointer(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    return priv->pb_stream.hw_ptr;
}

static const struct snd_pcm_ops playback_pcm_ops = {
    .open = playback_pcm_open,
    .close = playback_pcm_close,
    .hw_params = playback_pcm_hw_params,
    .hw_free = playback_pcm_hw_free,
    .prepare = playback_pcm_prepare,
    .trigger = playback_pcm_trigger,
    .pointer = playback_pcm_pointer,
};

static void pb_adj_read(struct snd_info_entry *entry, 
                       struct snd_info_buffer *buffer)
{
    struct motu_usb_data *priv = entry->private_data;

    snd_iprintf(buffer, "pb_adj: %d\n", READ_ONCE(priv->pb_adj_total));
}

static void pb_adj_write(struct snd_info_entry *entry,
                         struct snd_info_buffer *buffer)
{
    struct motu_usb_data *priv = entry->private_data;
    char line[16];
    if (!snd_info_get_line(buffer, line, sizeof(line))) {
        int pb_adj;
        if (!kstrtoint(line, 10, &pb_adj)) {
            dev_info(&priv->usb->dev, "pb_adj: %d\n", pb_adj);
            WRITE_ONCE(priv->pb_adj, pb_adj);
        }
    }
}

static void soft_reset_read(struct snd_info_entry *entry,
                            struct snd_info_buffer *buffer)
{
    snd_iprintf(buffer, "Write any value to trigger a soft reset of USB streams.\n");
}

static void soft_reset_write(struct snd_info_entry *entry,
                             struct snd_info_buffer *buffer)
{
    struct motu_usb_data *priv = entry->private_data;
    char line[16];

    if (!snd_info_get_line(buffer, line, sizeof(line))) {
        dev_info(&priv->usb->dev, "soft_reset triggered\n");
        restart_streaming_endpoints(priv);
    }
}

static void inject_gap_read(struct snd_info_entry *entry,
                            struct snd_info_buffer *buffer)
{
    snd_iprintf(buffer, "Write a gap length in ms (1-1000) to let the playback queue run dry once.\n");
}

static void inject_gap_write(struct snd_info_entry *entry,
                             struct snd_info_buffer *buffer)
{
    struct motu_usb_data *priv = entry->private_data;
    char line[16];
    unsigned int ms;

    if (snd_info_get_line(buffer, line, sizeof(line)))
        return;

    if (!atomic_read(&priv->streams_started)) {
        dev_info(&priv->usb->dev, "Test hook: not streaming, no gap injected\n");
        return;
    }

    /* as short as possible unless a length is given */
    if (kstrtouint(line, 10, &ms) || !ms)
        ms = 1;

    atomic_set(&priv->inject_gap, min(ms, 1000U));
}

static void stream_stats_read(struct snd_info_entry *entry,
                              struct snd_info_buffer *buffer)
{
    struct motu_usb_data *priv = entry->private_data;
    const char *last_gap = READ_ONCE(priv->last_gap);

    snd_iprintf(buffer, "streaming: %s\n",
        atomic_read(&priv->streams_started) ? "yes" : "no");
    snd_iprintf(buffer, "rate: %u\n", READ_ONCE(priv->cur_rate));
    snd_iprintf(buffer, "measured_rate: %u\n", READ_ONCE(priv->measured_rate));
    snd_iprintf(buffer, "scheduling: %s\n",
        READ_ONCE(priv->use_cfc) ? "cfc" : "iso_asap");
    snd_iprintf(buffer, "frame_check: %s\n", priv->frame_check ? "on" : "off");
    snd_iprintf(buffer, "auto_restart: %s\n", auto_restart ? "on" : "off");
    snd_iprintf(buffer, "gaps: %u\n", READ_ONCE(priv->gap_count));
    snd_iprintf(buffer, "gap_restarts: %u\n", READ_ONCE(priv->restart_count));
    snd_iprintf(buffer, "last_gap: %s\n", last_gap ? last_gap : "none");
}

/* Registered together with the card; the writable ones are root-only (0644). */
static void motu_create_proc_entry(struct motu_usb_data *priv)
{
    if (snd_card_ro_proc_new(priv->card, "stream_stats", priv,
                             stream_stats_read) ||
        snd_card_rw_proc_new(priv->card, "pb_adj", priv,
                             pb_adj_read, pb_adj_write) ||
        snd_card_rw_proc_new(priv->card, "soft_reset", priv,
                             soft_reset_read, soft_reset_write) ||
        (test_hooks &&
         snd_card_rw_proc_new(priv->card, "inject_gap", priv,
                              inject_gap_read, inject_gap_write)))
        dev_warn(&priv->usb->dev, "Failed to create proc entries\n");
}

static int pcm_init(struct motu_usb_data *priv)
{
    int err;
    struct snd_pcm *pcm;

    err = snd_pcm_new(priv->card, "MOTU Pro Audio", 0, 1, 1, &pcm);

    if (err)
        goto error;
    
    pcm->private_data = priv;
    priv->pcm = pcm;

    strscpy(pcm->name, "MOTU Pro Audio");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &capture_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &playback_pcm_ops);
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
    
error:
    return err;
}

/*
 * Return the xHCI capability registers of the host controller @udev is
 * attached to, or NULL if it is not an xHCI controller.
 */
static void __iomem *xhci_cap_regs(struct usb_device *udev)
{
    struct usb_hcd *hcd = bus_to_hcd(udev->bus);
    u16 hci_version;
    u32 cap;

    /* the register window belongs to the primary (USB 2) HCD */
    hcd = hcd->primary_hcd ?: hcd;

    /* only xHCI-based host controllers register as USB 3 */
    if ((hcd->driver->flags & HCD_MASK) < HCD_USB3 ||
        !(hcd->driver->flags & HCD_MEMORY) || !hcd->regs)
        return NULL;

    /* CAPLENGTH / HCIVERSION: make sure this is an xHCI register layout */
    cap = readl(hcd->regs);
    hci_version = cap >> 16;
    if (cap == ~0u || hci_version < 0x0096 || hci_version >= 0x0200)
        return NULL;

    return hcd->regs;
}

/*
 * 7.3-rc1 already reports version 7.3.0 but lacks the regression fix
 * 05506a76f13a, so check the release string as well.
 */
static bool cfc_kernel_ok(void)
{
    if (LINUX_VERSION_CODE < CFC_MIN_KERNEL)
        return false;

    return strncmp(init_utsname()->release, "7.3.0-rc1", 9) != 0;
}

/*
 * Decide whether the stream URBs are queued without URB_ISO_ASAP so the
 * xHCI driver schedules them on contiguous frame IDs. That needs a
 * controller with CFC (otherwise the flag is ignored, or on non-xHCI
 * controllers misinterpreted) and an xHCI driver that does not create gaps
 * of its own. Repeated gaps in CFC mode fall back to URB_ISO_ASAP at
 * runtime.
 */
static void select_iso_scheduling(struct motu_usb_data *priv)
{
    struct device *dev = &priv->usb->dev;
    void __iomem *regs = xhci_cap_regs(priv->usb);
    bool has_cfc = regs && (readl(regs + XHCI_HCCPARAMS1) & XHCI_HCC_CFC);
    bool kernel_ok = cfc_kernel_ok();

    switch (use_cfc) {
    case 0:
        priv->use_cfc = false;
        dev_info(dev, "Isochronous scheduling: URB_ISO_ASAP (use_cfc=0)\n");
        break;
    case 1:
        priv->use_cfc = has_cfc;
        if (has_cfc)
            dev_info(dev, "Isochronous scheduling: CFC (use_cfc=1)%s\n",
                kernel_ok ? "" : ", xHCI driver may still insert gaps");
        else
            dev_warn(dev, "use_cfc=1 ignored: %s, using URB_ISO_ASAP\n",
                regs ? "xHCI controller lacks CFC" : "not an xHCI controller");
        break;
    default:
        priv->use_cfc = has_cfc && kernel_ok;
        dev_info(dev, "Isochronous scheduling: %s (auto: controller %s CFC%s)\n",
            priv->use_cfc ? "CFC" : "URB_ISO_ASAP",
            has_cfc ? "supports" : "lacks",
            has_cfc && !kernel_ok ?
                ", xHCI driver not known to schedule it gap-free" : "");
        break;
    }
}

/*
 * Return the endpoint of alternate setting 1 of a streaming interface if it
 * is the isochronous endpoint of the expected direction, NULL otherwise.
 */
static const struct usb_endpoint_descriptor *
stream_endpoint(struct usb_interface *intf, bool dir_in)
{
    const struct usb_host_interface *alt = usb_altnum_to_altsetting(intf, 1);
    const struct usb_endpoint_descriptor *epd;

    if (!alt || alt->desc.bNumEndpoints < 1)
        return NULL;

    epd = &alt->endpoint[0].desc;

    if (!usb_endpoint_xfer_isoc(epd) || usb_endpoint_dir_in(epd) != dir_in ||
        !usb_endpoint_maxp(epd))
        return NULL;

    return epd;
}

/* The audio control interface carries the interrupt IN endpoint. */
static const struct usb_endpoint_descriptor *
interrupt_endpoint(struct usb_interface *intf)
{
    const struct usb_host_interface *alt = intf->cur_altsetting;

    if (alt->desc.bNumEndpoints < 1 ||
        !usb_endpoint_is_int_in(&alt->endpoint[0].desc))
        return NULL;

    return &alt->endpoint[0].desc;
}

static void motu_card_free(struct snd_card *card)
{
    struct motu_usb_data *priv = card->private_data;

    /* after a disconnect all of this is already gone */
    if (priv->start_stop_wq)
        destroy_workqueue(priv->start_stop_wq);

    free_interrupt_ubs(priv);
    free_stream_urbs(priv, &priv->rec_stream);
    free_stream_urbs(priv, &priv->pb_stream);
}

/*
 * Give back the interfaces of the card except @going, which is being
 * probed or unbound already. usb_driver_release_interface() calls
 * disconnect for each, which finds no intfdata and returns.
 */
static void release_interfaces(struct motu_usb_data *priv,
    struct usb_interface *going)
{
    struct usb_interface *intfs[] = {
        priv->ctrl_intf, priv->pb_intf, priv->rec_intf
    };

    priv->pb_intf = NULL;
    priv->rec_intf = NULL;

    for (int i = 0; i < ARRAY_SIZE(intfs); ++i)
        if (intfs[i])
            usb_set_intfdata(intfs[i], NULL);

    for (int i = 0; i < ARRAY_SIZE(intfs); ++i)
        if (intfs[i] && intfs[i] != going)
            usb_driver_release_interface(&snd_usb_motu_driver, intfs[i]);
}

static int claim_interface(struct motu_usb_data *priv,
    struct usb_interface *intf)
{
    int err = usb_driver_claim_interface(&snd_usb_motu_driver, intf, priv);

    if (err)
        dev_err(&priv->usb->dev,
            "Interface %d is in use by another driver (snd-usb-audio?)\n",
            intf->cur_altsetting->desc.bInterfaceNumber);

    return err;
}

static int motu_usb_audio_probe(struct usb_interface *intf,
            const struct usb_device_id *usb_id)
{
    struct snd_card *card;
    struct motu_usb_data *priv;
    struct usb_interface *pb_intf;
    struct usb_interface *rec_intf;
    const struct usb_endpoint_descriptor *int_epd;
    const struct usb_endpoint_descriptor *pb_epd;
    const struct usb_endpoint_descriptor *rec_epd;
    struct usb_device *dev = interface_to_usbdev(intf);
    int ifnum = intf->altsetting[0].desc.bInterfaceNumber;
    int err;
    int len;

    dev_info(&dev->dev, "Probing Interface: %d\n", ifnum);

    /* Audio Control Interface; the streaming interfaces are claimed below */
    if (ifnum != 0)
        return -ENODEV;

    validate_module_params(&dev->dev);

    if (!rate_flag(sample_rate)) {
        dev_err(&dev->dev, "Unsupported sample_rate: %d\n", sample_rate);
        return -EINVAL;
    }

    pb_intf = usb_ifnum_to_if(dev, 1);
    rec_intf = usb_ifnum_to_if(dev, 2);

    if (!pb_intf || !rec_intf) {
        dev_err(&dev->dev, "Streaming interfaces missing\n");
        return -ENODEV;
    }

    int_epd = interrupt_endpoint(intf);
    pb_epd = stream_endpoint(pb_intf, false);
    rec_epd = stream_endpoint(rec_intf, true);

    if (!int_epd || !pb_epd || !rec_epd) {
        dev_err(&dev->dev, "Unexpected interface layout (%s endpoint)\n",
            !int_epd ? "interrupt" : !pb_epd ? "playback" : "capture");
        return -ENODEV;
    }

    err = snd_card_new(&dev->dev, index, id ? id : "motuusb",
        THIS_MODULE, sizeof(*priv), &card);

    if (err)
        return err;

    DO_ONCE(init_shred_pattern);

    priv = card->private_data;
    priv->usb = dev;
    priv->card = card;
    priv->ctrl_intf = intf;
    priv->rec_stream.owner = priv;
    priv->pb_stream.owner = priv;

    spin_lock_init(&priv->rec_stream.lock);
    spin_lock_init(&priv->pb_stream.lock);
    spin_lock_init(&priv->wq_lock);
    mutex_init(&priv->params_lock);

    INIT_DELAYED_WORK(&priv->start_streams_work, start_streaming_endpoints);
    INIT_DELAYED_WORK(&priv->stop_streams_work, stop_streaming_endpoints);
    INIT_WORK(&priv->restart_work, restart_streaming_work);

    card->private_free = motu_card_free;

    /* usb_get_current_frame_number() only has a known period on xHCI */
    priv->frame_check = xhci_cap_regs(dev) != NULL;
    priv->restart_window = jiffies;
    select_iso_scheduling(priv);

    priv->start_stop_wq = alloc_ordered_workqueue("snd_usb_motu_wq", 0);

    if (!priv->start_stop_wq) {
        err = -ENOMEM;
        goto error_free;
    }

    err = alloc_interrupt_urbs(priv, int_epd, interrupt_complete_urb);

    if (err)
        goto error_free;
    
    err = alloc_stream_urbs(priv, &priv->pb_stream, pb_epd,
        playback_complete_urb);

    if (err)
        goto error_free;
    
    err = alloc_stream_urbs(priv, &priv->rec_stream, rec_epd,
        capture_complete_urb);

    if (err)
        goto error_free;

    for (int i = 0; i < ARRAY_SIZE(supported_rates); ++i)
        if (rate_fits_endpoints(priv, supported_rates[i]))
            priv->rates |= rate_flag(supported_rates[i]);

    err = set_runtime_rate(priv, sample_rate);
    if (err) {
        dev_err(&dev->dev, "sample_rate=%d does not fit the endpoints "
            "(max packet %u/%u bytes)\n", sample_rate,
            priv->pb_stream.max_packet_size,
            priv->rec_stream.max_packet_size);
        goto error_free;
    }
    
    err = pcm_init(priv);

    if (err)
        goto error_free;

    strscpy(card->driver, "MOTU Driver");
    /* several MOTU models share the USB ID: name the card after the product */
    snprintf(card->shortname, sizeof(card->shortname), "MOTU %s",
             dev->product ? dev->product : "Pro Audio");
    len = snprintf(card->longname, sizeof(card->longname), "%s at ",
                   card->shortname);
    if (len < sizeof(card->longname))
        usb_make_path(dev, card->longname + len, sizeof(card->longname) - len);
    
    motu_create_proc_entry(priv);

    err = claim_interface(priv, pb_intf);

    if (err)
        goto error_free;

    priv->pb_intf = pb_intf;

    err = claim_interface(priv, rec_intf);

    if (err)
        goto error_release;

    priv->rec_intf = rec_intf;

    usb_set_intfdata(intf, priv);

    err = snd_card_register(card);

    if (err)
        goto error_release;
    
    start_interrupt_urbs(priv);

    dev_info(&dev->dev, "Probe Success!\n");
    return 0;

error_release:
    release_interfaces(priv, intf);
error_free:
    dev_info(&dev->dev, "Probe Error: Freeing Card\n");
    snd_card_free(card);
    return err;
}

/*
 * Called for each of the card's interfaces. The first one to go tears the
 * card down and releases the others, whether the device was unplugged or
 * a single interface was unbound through sysfs.
 */
static void motu_usb_audio_disconnect(struct usb_interface *intf)
{
    struct motu_usb_data *priv = usb_get_intfdata(intf);
    struct workqueue_struct *wq;

    if (!priv)
        return;

    dev_info(&priv->usb->dev, "Disconnect: Freeing Card\n");

    WRITE_ONCE(priv->disconnected, true);
    snd_card_disconnect(priv->card);

    spin_lock_irq(&priv->wq_lock);
    wq = priv->start_stop_wq;
    priv->start_stop_wq = NULL;
    spin_unlock_irq(&priv->wq_lock);

    cancel_delayed_work_sync(&priv->start_streams_work);
    cancel_delayed_work_sync(&priv->stop_streams_work);
    cancel_work_sync(&priv->restart_work);
    destroy_workqueue(wq);

    /* back to alternate setting 0 if the device is still there */
    stop_streams(priv);

    free_interrupt_ubs(priv);
    free_stream_urbs(priv, &priv->rec_stream);
    free_stream_urbs(priv, &priv->pb_stream);

    release_interfaces(priv, intf);
    snd_card_free_when_closed(priv->card);
}

/*
 * Called for each of the card's interfaces; the first call stops
 * everything. Streams are not resumed: SNDRV_PCM_INFO_RESUME is not set,
 * so applications prepare and start them again after resume.
 */
static int motu_usb_audio_suspend(struct usb_interface *intf,
    pm_message_t message)
{
    struct motu_usb_data *priv = usb_get_intfdata(intf);
    bool already;

    if (!priv)
        return 0;

    if (PMSG_IS_AUTO(message) && atomic_read(&priv->streams_started))
        return -EBUSY;

    /*
     * Block new work first: a gap report or a soft_reset write could
     * otherwise queue a restart that starts the streams again behind us.
     */
    spin_lock_irq(&priv->wq_lock);
    already = priv->suspended;
    priv->suspended = true;
    spin_unlock_irq(&priv->wq_lock);

    if (already)
        return 0;

    snd_power_change_state(priv->card, SNDRV_CTL_POWER_D3hot);
    snd_pcm_suspend_all(priv->pcm);

    /* a restart already running may start the streams; stop them after */
    cancel_delayed_work_sync(&priv->start_streams_work);
    cancel_delayed_work_sync(&priv->stop_streams_work);
    cancel_work_sync(&priv->restart_work);
    stop_streams(priv);

    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i)
        usb_kill_urb(priv->interrupt.urbs[i]);

    return 0;
}

/* Also used for reset_resume: stream start reprograms the device. */
static int motu_usb_audio_resume(struct usb_interface *intf)
{
    struct motu_usb_data *priv = usb_get_intfdata(intf);
    bool suspended;

    if (!priv)
        return 0;

    spin_lock_irq(&priv->wq_lock);
    suspended = priv->suspended;
    priv->suspended = false;
    spin_unlock_irq(&priv->wq_lock);

    if (!suspended)
        return 0;

    priv->interrupt.urbs_idle = 0;
    start_interrupt_urbs(priv);
    snd_power_change_state(priv->card, SNDRV_CTL_POWER_D0);

    return 0;
}

static const struct usb_device_id motu_usb_ids[] = {
    /* MOTU Pro Audio Devices: bind the control interface, claim the rest */
    { USB_DEVICE_INTERFACE_NUMBER(0x07fd, 0x0005, 0) },
    { }
};

MODULE_DEVICE_TABLE(usb, motu_usb_ids);

static struct usb_driver snd_usb_motu_driver = {
    .name = "snd-usb-motu",
    .id_table = motu_usb_ids,
    .probe = motu_usb_audio_probe,
    .disconnect = motu_usb_audio_disconnect,
    .suspend = motu_usb_audio_suspend,
    .resume = motu_usb_audio_resume,
    .reset_resume = motu_usb_audio_resume,
};

module_usb_driver(snd_usb_motu_driver);