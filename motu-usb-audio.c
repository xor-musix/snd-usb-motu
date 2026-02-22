// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU Pro Audio USB Driver
 */
#include <linux/module.h>
#include <linux/once.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/usb.h>
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
 */
static int sample_rate = 48000;
module_param(sample_rate, int, 0644);
MODULE_PARM_DESC(sample_rate, "Sample rate in Hz (44100, 48000, 88200, 96000, 176400, 192000)");

static unsigned int uframes_per_urb = 8;
module_param(uframes_per_urb, uint, 0444);
MODULE_PARM_DESC(uframes_per_urb,
    "Microframes per URB (1,2,4,8). Lower=less latency, higher=less CPU. Default: 8");

#define NUM_INTERRUPT_URBS  4
#define MAX_NUM_URBS        256
#define MAX_UFRAMES_PER_URB 8
#define MAX_TOTAL_UFRAMES   256
#define NUM_CH              24
#define BYTES_PER_SAMPLE    3
#define BYTES_PER_FRAME     (NUM_CH * BYTES_PER_SAMPLE)
#define PB_SAFETY_OFFSET    4
#define REC_SAFETY_OFFSET   4

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
    case 48000:
        return 6;
    case 88200:
        return 11;
    case 96000:
        return 12;
    case 176400:
        return 22;
    case 192000:
        return 24;
    default:
        return -EINVAL;
    }
}

/* Mask of all supported rates for initial hw template; narrowed at open() */
#define SUPPORTED_RATES_MASK ( \
    SNDRV_PCM_RATE_44100  | \
    SNDRV_PCM_RATE_48000  | \
    SNDRV_PCM_RATE_88200  | \
    SNDRV_PCM_RATE_96000  | \
    SNDRV_PCM_RATE_176400 | \
    SNDRV_PCM_RATE_192000   \
)


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
    struct motu_buf bufs[MAX_TOTAL_UFRAMES];
    struct urb *urbs[MAX_NUM_URBS];
};

struct motu_interrupt
{
    unsigned int interval;
    struct urb *urbs[NUM_INTERRUPT_URBS];
};

struct motu_usb_data
{
    spinlock_t wq_lock;
    atomic_t streams_started;
    struct usb_device *usb;
    struct snd_card *card;
    struct workqueue_struct *start_stop_wq;
    struct delayed_work start_streams_work;
    struct delayed_work stop_streams_work;
    struct work_struct xrun_work;
    struct motu_interrupt interrupt;
    struct motu_stream rec_stream;
    struct motu_stream pb_stream;
    unsigned int urb_idx;
    unsigned int rx_frames;
    int pb_adj;
    int pb_adj_total;
    unsigned int zero_frame_count;
    unsigned int recovery_attempts;
    bool disconnected;
    struct mutex pcm_mutex;
    enum pb_start_state pb_start_state;
    /* Runtime sample-rate state */
    unsigned int cur_rate;        /* 0 until set; in Hz */
    int nom_sample_count;         /* derived from cur_rate */
    /* Runtime USB scheduling parameters (from module param) */
    unsigned int rt_uframes_per_urb;
    unsigned int rt_num_urbs;
    unsigned int rt_num_urbs_mask;
    unsigned int rt_total_uframes;
    unsigned int rt_total_uframes_mask;
    unsigned int rt_rec_safety_offset;
};

struct motu_interrupt_msg
{
    unsigned char info;
    unsigned char attr;
    unsigned int frame;
} __attribute__((packed));

static int set_runtime_rate(struct motu_usb_data *priv, unsigned int rate)
{
    int nsc;

    if (!rate_flag(rate))
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
             SNDRV_PCM_INFO_INTERLEAVED),
    .formats =          SNDRV_PCM_FMTBIT_S24_3LE,
    .rates =            SUPPORTED_RATES_MASK,
    .rate_min =         44100,
    .rate_max =         192000,
    .channels_min =     NUM_CH,
    .channels_max =     NUM_CH,
    .buffer_bytes_max = BYTES_PER_FRAME * 2048 * 4,
    .period_bytes_min = BYTES_PER_FRAME * 16,
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
    if (!stream->period_frames)
        return false;

    stream->frame_pos += count;

    if (stream->frame_pos >= stream->period_frames) {
        stream->frame_pos %= stream->period_frames;
        
        stream->period_idx = (stream->period_idx + 1) % stream->period_count;
        
        stream->hw_ptr = stream->period_idx * stream->period_frames + stream->frame_pos;
        
        return true;
    }

    /* Sub-period hw_ptr tracking */
    stream->hw_ptr = (stream->period_idx * stream->period_frames) +
                     stream->frame_pos;

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
 * copy_frames_from_usb - Incrementally copy capture frames to ALSA buffer
 * @stream: capture stream
 * @frames: number of frames to copy from current buf position
 *
 * Copies frames and signals period elapsed when a full period is accumulated.
 * Must be called with stream->lock held.
 */
static void copy_frames_from_usb(struct motu_stream *stream, unsigned int frames)
{
    unsigned char *dst;
    unsigned int dst_offset;
    unsigned int buffer_size_bytes;

    if (!stream->enabled || !stream->substream || !stream->substream->runtime ||
        !stream->period_frames)
        return;

    dst = stream->substream->runtime->dma_area;
    buffer_size_bytes = stream->period_count * stream->period_frames * BYTES_PER_FRAME;

    if (!dst || !buffer_size_bytes)
        return;

    while (frames) {
        struct motu_buf *src_buf = &stream->bufs[stream->copy_pos];
        unsigned int src_frames = src_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(frames, src_frames);
        unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
        unsigned int src_offset = stream->copy_frame * BYTES_PER_FRAME;

        if (!frames_this_copy)
            break;

        dst_offset = substream_position_bytes(stream);
        
        if (dst_offset >= buffer_size_bytes) {
            /* Should not happen with correct inc_frame_position logic, but safety first */
            reset_substream_position(stream);
            dst_offset = 0;
        }

        /* Copy only up to the end of the buffer; next iteration handles wrap */
        if ((dst_offset + bytes_this_copy) > buffer_size_bytes) {
            bytes_this_copy = buffer_size_bytes - dst_offset;
            frames_this_copy = bytes_this_copy / BYTES_PER_FRAME;
        }

        if (bytes_this_copy > 0)
            memcpy(dst + dst_offset, src_buf->data + src_offset, bytes_this_copy);

        frames -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= src_buf->length) {
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) & stream->owner->rt_total_uframes_mask;
        }

        if (inc_frame_position(stream, frames_this_copy))
            snd_pcm_period_elapsed(stream->substream);
    }
}

/*
 * copy_frames_to_usb - Incrementally copy playback frames from ALSA buffer
 * @stream: playback stream
 * @frames: number of frames to fill in current buf position
 *
 * Copies frames and signals period elapsed when a full period is consumed.
 * Must be called with stream->lock held.
 */
static void copy_frames_to_usb(struct motu_stream *stream, unsigned int frames)
{
    unsigned char *src;
    unsigned int src_offset;
    unsigned int buffer_size_bytes;

    if (!stream->enabled || !stream->substream || !stream->substream->runtime ||
        !stream->period_frames) {
        /* Mute: zero out the playback buffers */
        while (frames) {
            struct motu_buf *dst_buf = &stream->bufs[stream->copy_pos];
            unsigned int dst_frames = dst_buf->length - stream->copy_frame;
            unsigned int frames_this_copy = min(frames, dst_frames);
            unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
            unsigned int dst_offset = stream->copy_frame * BYTES_PER_FRAME;

            memset(dst_buf->data + dst_offset, 0, bytes_this_copy);

            frames -= frames_this_copy;

            stream->copy_frame += frames_this_copy;
            if (stream->copy_frame >= dst_buf->length) {
                stream->copy_frame = 0;
                stream->copy_pos = (stream->copy_pos + 1) & stream->owner->rt_total_uframes_mask;
            }
        }
        return;
    }

    src = stream->substream->runtime->dma_area;
    buffer_size_bytes = stream->period_count * stream->period_frames * BYTES_PER_FRAME;

    if (!src || !buffer_size_bytes)
        return;

    while (frames) {
        struct motu_buf *dst_buf = &stream->bufs[stream->copy_pos];
        unsigned int dst_frames = dst_buf->length - stream->copy_frame;
        unsigned int frames_this_copy = min(frames, dst_frames);
        unsigned int bytes_this_copy = frames_this_copy * BYTES_PER_FRAME;
        unsigned int dst_offset = stream->copy_frame * BYTES_PER_FRAME;

        if (!frames_this_copy)
            break;

        src_offset = substream_position_bytes(stream);
        
        if (src_offset >= buffer_size_bytes) {
            reset_substream_position(stream);
            src_offset = 0;
        }

        /* Copy only up to the end of the buffer; next iteration handles wrap */
        if ((src_offset + bytes_this_copy) > buffer_size_bytes) {
            bytes_this_copy = buffer_size_bytes - src_offset;
            frames_this_copy = bytes_this_copy / BYTES_PER_FRAME;
        }

        if (bytes_this_copy > 0)
            memcpy(dst_buf->data + dst_offset, src + src_offset, bytes_this_copy);

        frames -= frames_this_copy;

        stream->copy_frame += frames_this_copy;
        if (stream->copy_frame >= dst_buf->length) {
            stream->copy_frame = 0;
            stream->copy_pos = (stream->copy_pos + 1) & stream->owner->rt_total_uframes_mask;
        }

        if (inc_frame_position(stream, frames_this_copy))
            snd_pcm_period_elapsed(stream->substream);
    }
}

#define MAX_RECOVERY_ATTEMPTS 3

static void restart_streaming_endpoints(struct motu_usb_data *priv);

static void handle_status_interrupt(struct class_interrupt_msg *msg,
    struct motu_usb_data *priv)
{
    dev_info(&priv->usb->dev,
        "status: info:0x%02x attr:0x%02x cn:%u cs:%u intf:%u id:%u\n",
        msg->info, msg->attr, msg->cn, msg->cs, msg->intf, msg->id);

    if (atomic_read(&priv->streams_started))
        restart_streaming_endpoints(priv);
}

static void interrupt_complete_urb(struct urb* urb)
{
    struct motu_interrupt_msg *msg = urb->transfer_buffer;
    struct motu_usb_data *priv = urb->context;

    if (!priv || priv->disconnected)
        return;

    if (urb->status == 0) {
        if (!(msg->info == 0x01 && msg->attr == 0x01))
            handle_status_interrupt(urb->transfer_buffer, priv);
    }

    usb_submit_urb(urb, GFP_ATOMIC);
}

static void playback_complete_urb(struct urb *urb)
{
    /*
    struct motu_usb_data *priv = urb->context;
    dev_info(&priv->usb->dev, "copy_pos:%u\n", priv->pb_stream.copy_pos);
    */
}

static void xrun_work_fn(struct work_struct *work)
{
    struct motu_usb_data *priv =
        container_of(work, struct motu_usb_data, xrun_work);

    if (priv->rec_stream.substream)
        snd_pcm_stop_xrun(priv->rec_stream.substream);
    if (priv->pb_stream.substream)
        snd_pcm_stop_xrun(priv->pb_stream.substream);
}

static void capture_complete_urb(struct urb *urb)
{
    struct motu_usb_data *priv = urb->context;
    struct motu_stream *rec_stream = &priv->rec_stream;
    struct motu_stream *pb_stream = &priv->pb_stream;
    struct urb *pb_urb;
    struct motu_buf *rec_bufs;
    struct motu_buf *pb_bufs;
    unsigned int urb_idx;
    unsigned int urb_frames = 0;

    if (!priv || 
        priv->disconnected || 
        !atomic_read(&priv->streams_started))
        return;
    
    urb_idx = priv->urb_idx;
    pb_urb = pb_stream->urbs[urb_idx];
    rec_bufs = &rec_stream->bufs[urb_idx * priv->rt_uframes_per_urb];
    pb_bufs = &pb_stream->bufs[urb_idx * priv->rt_uframes_per_urb];
    priv->urb_idx = (priv->urb_idx + 1) & priv->rt_num_urbs_mask;
    
    /* Use actual_length to determine frame counts directly */
    for (int i = 0; i < priv->rt_uframes_per_urb; ++i) {
        unsigned int length = BYTES_PER_FRAME * priv->nom_sample_count;
        
        if (!urb->iso_frame_desc[i].status)
            length = urb->iso_frame_desc[i].actual_length;
        else
            dev_info(&priv->usb->dev, "rec urb error\n");
        
        pb_urb->iso_frame_desc[i].length = length;
        rec_bufs[i].length = length / BYTES_PER_FRAME;
        pb_bufs[i].length = length / BYTES_PER_FRAME;
        urb_frames += length / BYTES_PER_FRAME;
    }

    priv->rx_frames += urb_frames;

    /* Zero-frame watchdog: detect clock instability */
    if (urb_frames == 0) {
        priv->zero_frame_count++;
        if (priv->zero_frame_count >= priv->rt_num_urbs) {
            priv->zero_frame_count = 0;

            if (priv->recovery_attempts < MAX_RECOVERY_ATTEMPTS) {
                priv->recovery_attempts++;
                dev_warn(&priv->usb->dev,
                    "Clock loss detected, recovery attempt %u/%u\n",
                    priv->recovery_attempts, MAX_RECOVERY_ATTEMPTS);
                restart_streaming_endpoints(priv);
            } else {
                dev_err(&priv->usb->dev,
                    "Clock loss: recovery failed after %u attempts, signaling xrun\n",
                    MAX_RECOVERY_ATTEMPTS);
                priv->recovery_attempts = 0;
                schedule_work(&priv->xrun_work);
            }
        }
    } else {
        priv->zero_frame_count = 0;
        priv->recovery_attempts = 0;
    }

    /* Resubmit capture URB immediately */
    if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
        dev_err(&priv->usb->dev, "Capture URB resubmit failed\n");
        schedule_work(&priv->xrun_work);
        return;
    }

    /* Capture: copy received audio data to ALSA buffer */
    if (rec_stream->copy_pos == priv->rt_total_uframes) {
        /* Capture startup - wait for enough data */
        if (priv->rx_frames >= (priv->rt_rec_safety_offset + urb_frames)) {
            rec_stream->copy_pos = urb_idx * priv->rt_uframes_per_urb;
            rec_stream->copy_frame = 0;
            dev_info(&priv->usb->dev,
                "Rec Copy Sync: %u (count: %u)\n",
                rec_stream->copy_pos, priv->rx_frames);
        }
    }

    if (rec_stream->copy_pos != priv->rt_total_uframes) {
        spin_lock(&rec_stream->lock);
        copy_frames_from_usb(rec_stream, urb_frames);
        spin_unlock(&rec_stream->lock);
    }

    /* Playback adjustment from handle_interval_interrupt */
    if (priv->pb_adj) {
        spin_lock(&pb_stream->lock);
        pb_stream->copy_pos =
            (pb_stream->copy_pos + priv->pb_adj) & priv->rt_total_uframes_mask;
        priv->pb_adj_total += priv->pb_adj;
        priv->pb_adj = 0;
        spin_unlock(&pb_stream->lock);
    }

    /* Playback: fill outgoing URB from ALSA buffer */
    switch (priv->pb_start_state) {
    case PB_START_IDLE:
        priv->pb_start_state = PB_START_PRIME;
        break;
    case PB_START_PRIME:
        /* Pre-fill all playback URBs with silence before first submit */
        for (int i = 0; i < priv->rt_num_urbs; ++i) {
            struct urb *u = pb_stream->urbs[i];
            if (u && u->transfer_buffer)
                memset(u->transfer_buffer, 0, u->transfer_buffer_length);
        }
        dev_info(&priv->usb->dev,
            "PB Prime: prefilled %u URBs with silence\n", priv->rt_num_urbs);
        spin_lock(&pb_stream->lock);
        pb_stream->copy_pos = 0;
        pb_stream->copy_frame = 0;
        copy_frames_to_usb(pb_stream, priv->rt_uframes_per_urb);
        usb_submit_urb(pb_stream->urbs[0], GFP_ATOMIC);
        spin_unlock(&pb_stream->lock);
        priv->pb_start_state = PB_START_SYNC;
        break;
    case PB_START_SYNC:
        /* Sync playback copy position relative to current capture position */
        pb_stream->copy_pos = urb_idx * priv->rt_uframes_per_urb;
        pb_stream->copy_frame = 0;
        priv->pb_start_state = PB_START_RUNNING;
        dev_info(&priv->usb->dev,
            "PB Copy Sync: %u (idx: %u)\n", pb_stream->copy_pos, urb_idx);
        fallthrough;
    case PB_START_RUNNING:
        spin_lock(&pb_stream->lock);
        copy_frames_to_usb(pb_stream, urb_frames);
        spin_unlock(&pb_stream->lock);
        if (usb_submit_urb(pb_urb, GFP_ATOMIC) < 0) {
            dev_err(&priv->usb->dev, "Playback URB submit failed\n");
            schedule_work(&priv->xrun_work);
        }
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

    if (ret < 0)
        dev_warn(&priv->usb->dev,
            "Failed to set interrupt interval: %d\n", ret);
}

static void restart_streaming_endpoints(struct motu_usb_data *priv)
{
    spin_lock(&priv->wq_lock);

    if (priv->start_stop_wq) {
        queue_delayed_work(priv->start_stop_wq,
            &priv->stop_streams_work, 0);
        queue_delayed_work(priv->start_stop_wq,
            &priv->start_streams_work, msecs_to_jiffies(1));
    }

    spin_unlock(&priv->wq_lock);
}

static void start_streaming_endpoints(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct motu_usb_data *priv =
        container_of(dwork, struct motu_usb_data, start_streams_work);
    unsigned int interval =
        max(priv->rec_stream.period_frames,
            priv->pb_stream.period_frames);
    struct urb **urbs;

    if (atomic_cmpxchg(&priv->streams_started, 0, 1)) {
        if (interval != priv->interrupt.interval)
            restart_streaming_endpoints(priv);
        return;
    }
    
    dev_info(&priv->usb->dev, "start_streaming_endpoints\n");
    
    urbs = priv->rec_stream.urbs;

    priv->rec_stream.copy_pos = priv->rt_total_uframes;
    priv->rec_stream.copy_frame = 0;
    priv->pb_stream.copy_pos = priv->rt_total_uframes;
    priv->pb_stream.copy_frame = 0;
    priv->urb_idx = 0;
    priv->rx_frames = 0;
    priv->zero_frame_count = 0;
    priv->recovery_attempts = 0;
    priv->pb_start_state = PB_START_IDLE;

    for (int i = 0; i < priv->rt_total_uframes; ++i) {
        priv->rec_stream.bufs[i].length = 0;
        priv->pb_stream.bufs[i].length = 0;
    }

    for (int i = 0; i < priv->rt_total_uframes; ++i)
        shred_sample_frames(&priv->rec_stream, priv->rec_stream.bufs[i].data);

    set_interrupt_interval(priv, 1);
    usb_set_interface(priv->usb, 2, 1); // start record
    usb_set_interface(priv->usb, 1, 1); // start playback

    for (int i = 0; i < priv->rt_num_urbs; ++i)
        usb_submit_urb(urbs[i], GFP_ATOMIC);
}

static void stop_streaming_endpoints(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct motu_usb_data *priv =
        container_of(dwork, struct motu_usb_data, stop_streams_work);
    
    if (!atomic_cmpxchg(&priv->streams_started, 1, 0))
       return;
    
    dev_info(&priv->usb->dev, "stop_streaming_endpoints\n");

    priv->pb_adj_total = 0;

    set_interrupt_interval(priv, 0);

    for (int i = 0; i < priv->rt_num_urbs; ++i) {
        usb_kill_urb(priv->rec_stream.urbs[i]);
        usb_kill_urb(priv->pb_stream.urbs[i]);
    }
    
    usb_set_interface(priv->usb, 2, 0); // stop record
    usb_set_interface(priv->usb, 1, 0); // stop playback
}

static void free_stream_urbs(struct motu_usb_data *priv,
    struct motu_stream *stream)
{
    struct urb **urbs = stream->urbs;

    for (int i = 0; i < MAX_NUM_URBS; ++i) {
        
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

static int alloc_stream_urbs(struct motu_usb_data *priv,
            struct motu_stream *stream, struct usb_interface *intf,
            usb_complete_t completion_handler)
{
    const struct usb_host_interface *alt = usb_altnum_to_altsetting(intf, 1);
    const struct usb_endpoint_descriptor *epd = &alt->endpoint[0].desc;
    struct urb **urbs = stream->urbs;
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
    transfer_buffer_length = max_packet_size * priv->rt_uframes_per_urb;

    for (int i = 0; i < priv->rt_num_urbs; ++i) {
        unsigned int buf_base;

        urbs[i] = usb_alloc_urb(priv->rt_uframes_per_urb, GFP_KERNEL);
        
        if (!urbs[i])
            goto out_free;

        transfer_buffer = usb_alloc_coherent(priv->usb,
                            transfer_buffer_length,
                            GFP_KERNEL, &transfer_dma);
        
        if (!transfer_buffer)
            goto out_free;
        
        urbs[i]->dev = priv->usb;
        urbs[i]->pipe = pipe;
        urbs[i]->transfer_flags = URB_NO_TRANSFER_DMA_MAP | URB_ISO_ASAP;
        urbs[i]->transfer_buffer = transfer_buffer;
        urbs[i]->transfer_dma = transfer_dma;
        urbs[i]->transfer_buffer_length = max_packet_size * priv->rt_uframes_per_urb;
        urbs[i]->number_of_packets = priv->rt_uframes_per_urb;
        urbs[i]->interval = 1;
        urbs[i]->context = priv;
        urbs[i]->complete = completion_handler;

        buf_base = i * priv->rt_uframes_per_urb;

        for (int f = 0; f < priv->rt_uframes_per_urb; ++f) {
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
            struct usb_interface *intf, usb_complete_t completion_handler)
{
    struct urb **urbs = priv->interrupt.urbs;
    struct usb_endpoint_descriptor *epd = &intf->altsetting[0].endpoint[0].desc;
    unsigned int pipe = usb_rcvintpipe(priv->usb, usb_endpoint_num(epd));

    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i) {
        urbs[i] = usb_alloc_urb(0, GFP_KERNEL);

        if (!urbs[i])
            goto out_free;

        urbs[i]->dev = priv->usb;
        urbs[i]->pipe = pipe;
        urbs[i]->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
        urbs[i]->transfer_buffer = usb_alloc_coherent(priv->usb, 6, GFP_KERNEL,
                                        &urbs[i]->transfer_dma);
        
        if (!urbs[i]->transfer_buffer)
            goto out_free;
        
        urbs[i]->transfer_buffer_length = 6;
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
    for (int i = 0; i < NUM_INTERRUPT_URBS; ++i)
        usb_submit_urb(priv->interrupt.urbs[i], GFP_ATOMIC);
}

static void queue_start_streaming(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    if (priv->disconnected)
        return;

    cancel_delayed_work(&priv->stop_streams_work);
    
    spin_lock(&priv->wq_lock);
    if (priv->start_stop_wq)
        queue_delayed_work(priv->start_stop_wq, &priv->start_streams_work, 0);
    spin_unlock(&priv->wq_lock);
}

static void maybe_queue_stop_streaming(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    if (priv->disconnected)
        return;

    if (priv->rec_stream.enabled || priv->pb_stream.enabled)
        return;

    cancel_delayed_work(&priv->start_streams_work);
    
    spin_lock(&priv->wq_lock);
    if (priv->start_stop_wq)
        queue_delayed_work(priv->start_stop_wq, &priv->stop_streams_work,
            msecs_to_jiffies(100));
    spin_unlock(&priv->wq_lock);
}

static int capture_pcm_open(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    if (priv->disconnected)
        return -ENODEV;

    dev_info(&priv->usb->dev, "capture_pcm_open\n");

    subs->runtime->hw = snd_motu_hw;

    /* Enforce minimum period size based on USB scheduling granularity.
     * Use worst-case nom_sample_count (24 for 192kHz) if rate not yet set. */
    {
        unsigned int nsc = priv->nom_sample_count ? priv->nom_sample_count : 24;
        unsigned int min_period = priv->rt_uframes_per_urb * nsc;
        snd_pcm_hw_constraint_minmax(subs->runtime,
            SNDRV_PCM_HW_PARAM_PERIOD_SIZE, min_period, 2048);
        dev_info(&priv->usb->dev, "min period size set to %u \n", min_period);
    }

    spin_lock(&priv->rec_stream.lock);
    priv->rec_stream.substream = subs;
    priv->rec_stream.period_frames = 0;
    spin_unlock(&priv->rec_stream.lock);

    return 0;
}

static int capture_pcm_close(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    spin_lock(&priv->rec_stream.lock);
    priv->rec_stream.substream = NULL;
    priv->rec_stream.period_frames = 0;
    spin_unlock(&priv->rec_stream.lock);

    return 0;
}

static int capture_pcm_hw_params(struct snd_pcm_substream *subs,
            struct snd_pcm_hw_params *hw_params)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned int rate = params_rate(hw_params);
    int err;

    if (priv->disconnected)
        return -ENODEV;

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

    if (!rate_flag(rate))
        return -EINVAL;

    if (!priv->cur_rate || priv->cur_rate != rate) {
        bool running = atomic_read(&priv->streams_started);
        err = set_runtime_rate(priv, rate);
        if (err)
            return err;
        if (running)
            restart_streaming_endpoints(priv);
    }

    if (priv->pb_stream.period_frames &&
        priv->pb_stream.period_frames != params_period_size(hw_params)) {
        
        dev_info(&priv->usb->dev,
            "Capture and Playback must use same period size!\n");
        
        return -EINVAL;
    }
    
    spin_lock(&priv->rec_stream.lock);
    reset_substream_position(&priv->rec_stream);
    priv->rec_stream.period_count = params_periods(hw_params);
    priv->rec_stream.period_frames = params_period_size(hw_params);
    spin_unlock(&priv->rec_stream.lock);
    
    return 0;
}

static int capture_pcm_hw_free(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "capture_pcm_hw_free\n");

    spin_lock(&priv->rec_stream.lock);
    priv->rec_stream.period_frames = 0;
    spin_unlock(&priv->rec_stream.lock);

    priv->rec_stream.enabled = false;
    maybe_queue_stop_streaming(subs);

    return 0;
}

static int capture_pcm_prepare(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    if (priv->disconnected)
        return -ENODEV;

    dev_info(&priv->usb->dev, "capture_pcm_prepare\n");

    spin_lock(&priv->rec_stream.lock);
    reset_substream_position(&priv->rec_stream);
    spin_unlock(&priv->rec_stream.lock);

    return 0;
}

static int capture_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
    struct motu_usb_data *priv = subs->private_data;

    if (priv->disconnected)
        return -ENODEV;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        dev_info(&priv->usb->dev,
            "capture_pcm_trigger start (stop_threshold:%lu)\n",
            subs->runtime->stop_threshold);
        priv->rec_stream.enabled = true;
        queue_start_streaming(subs);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        dev_info(&priv->usb->dev, "capture_pcm_trigger stop\n");
        priv->rec_stream.enabled = false;
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

    subs->runtime->hw = snd_motu_hw;

    /* Enforce minimum period size based on USB scheduling granularity.
     * Use worst-case nom_sample_count (24 for 192kHz) if rate not yet set. */
    {
        unsigned int nsc = priv->nom_sample_count ? priv->nom_sample_count : 24;
        unsigned int min_period = priv->rt_uframes_per_urb * nsc;
        snd_pcm_hw_constraint_minmax(subs->runtime,
            SNDRV_PCM_HW_PARAM_PERIOD_SIZE, min_period, 2048);
        dev_info(&priv->usb->dev, "min period size set to %u \n", min_period);
    }

    spin_lock(&priv->pb_stream.lock);
    priv->pb_stream.substream = subs;
    priv->pb_stream.period_frames = 0;
    spin_unlock(&priv->pb_stream.lock);

    return 0;
}

static int playback_pcm_close(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_close\n");

    spin_lock(&priv->pb_stream.lock);
    priv->pb_stream.substream = NULL;
    priv->pb_stream.period_frames = 0;
    spin_unlock(&priv->pb_stream.lock);

    return 0;
}

static int playback_pcm_hw_params(struct snd_pcm_substream *subs,
            struct snd_pcm_hw_params *hw_params)
{
    struct motu_usb_data *priv = subs->private_data;
    unsigned int rate = params_rate(hw_params);
    int err;

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

    if (!rate_flag(rate))
        return -EINVAL;

    if (!priv->cur_rate || priv->cur_rate != rate) {
        bool running = atomic_read(&priv->streams_started);
        err = set_runtime_rate(priv, rate);
        if (err)
            return err;
        if (running)
            restart_streaming_endpoints(priv);
    }

    if (priv->rec_stream.period_frames &&
        priv->rec_stream.period_frames != params_period_size(hw_params)) {
        
        dev_info(&priv->usb->dev,
            "Capture and Playback must use same period size!\n");
        
        return -EINVAL;
    }
    
    spin_lock(&priv->pb_stream.lock);
    reset_substream_position(&priv->pb_stream);
    priv->pb_stream.period_count = params_periods(hw_params);
    priv->pb_stream.period_frames = params_period_size(hw_params);
    spin_unlock(&priv->pb_stream.lock);
    
    return 0;
}

static int playback_pcm_hw_free(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_hw_free\n");

    spin_lock(&priv->pb_stream.lock);
    priv->pb_stream.period_frames = 0;
    spin_unlock(&priv->pb_stream.lock);

    priv->pb_stream.enabled = false;
    maybe_queue_stop_streaming(subs);

    return 0;
}

static int playback_pcm_prepare(struct snd_pcm_substream *subs)
{
    struct motu_usb_data *priv = subs->private_data;

    dev_info(&priv->usb->dev, "playback_pcm_prepare\n");

    spin_lock(&priv->pb_stream.lock);
    reset_substream_position(&priv->pb_stream);
    spin_unlock(&priv->pb_stream.lock);

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
        priv->pb_stream.enabled = true;
        queue_start_streaming(subs);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        dev_info(&priv->usb->dev, "playback_pcm_trigger stop\n");
        priv->pb_stream.enabled = false;
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

    snd_iprintf(buffer, "pb_adj: %d\n", priv->pb_adj_total);
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
            priv->pb_adj = pb_adj;
        }
    }
}

static void motu_create_proc_entry(struct motu_usb_data *priv)
{
    struct snd_info_entry *entry;

    entry = snd_info_create_card_entry(priv->card, "pb_adj",
        priv->card->proc_root);
    
    if (!entry)
        return;

    entry->private_data = priv;
    entry->c.text.read = pb_adj_read;
    entry->c.text.write = pb_adj_write;
    
    snd_info_register(entry);
}

static int pcm_init(struct motu_usb_data *priv)
{
    int err;
    struct snd_pcm *pcm;

    err = snd_pcm_new(priv->card, "MOTU Pro Audio", 0, 1, 1, &pcm);

    if (err)
        goto error;
    
    pcm->private_data = priv;

    strcpy(pcm->name, "MOTU Pro Audio");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &capture_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &playback_pcm_ops);
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
    
error:
    return err;
}

static int motu_usb_audio_probe(struct usb_interface *intf,
            const struct usb_device_id *usb_id)
{
    struct snd_card *card;
    struct motu_usb_data *priv;
    struct usb_interface *pb_intf;
    struct usb_interface *rec_intf;
    struct usb_device *dev = interface_to_usbdev(intf);
    int ifnum = intf->altsetting[0].desc.bInterfaceNumber;
    int err = 0;

    dev_info(&dev->dev, "Probing Interface: %d\n", ifnum);
    
    /* Audio Control Interface */
    if (ifnum == 0) {
        err = snd_card_new(&dev->dev, SNDRV_DEFAULT_IDX1, "motuusb",
            THIS_MODULE, sizeof(*priv), &card);
        
        if (err)
            return err;
        
        DO_ONCE(init_shred_pattern);

        /* Initialize runtime sample rate from module parameter (default). */
        if (!rate_flag(sample_rate)) {
            dev_err(&dev->dev, "Unsupported sample_rate: %d\n", sample_rate);
            err = -EINVAL;
            goto error_free_card_direct;
        }
        
        priv = card->private_data;
        priv->usb = dev;
        priv->card = card;
        priv->rec_stream.owner = priv;
        priv->pb_stream.owner = priv;
        priv->disconnected = false;

        /* Validate and apply USB scheduling module parameter */
        if (uframes_per_urb != 1 && uframes_per_urb != 2 &&
            uframes_per_urb != 4 && uframes_per_urb != 8) {
            dev_warn(&dev->dev,
                "Invalid uframes_per_urb=%u, clamping to 8\n",
                uframes_per_urb);
            uframes_per_urb = 8;
        }
        priv->rt_uframes_per_urb = uframes_per_urb;
        priv->rt_num_urbs = 256 / uframes_per_urb;
        priv->rt_total_uframes = priv->rt_num_urbs * priv->rt_uframes_per_urb;
        priv->rt_num_urbs_mask = priv->rt_num_urbs - 1;
        priv->rt_total_uframes_mask = priv->rt_total_uframes - 1;
        priv->rt_rec_safety_offset = max(4u, uframes_per_urb);
        dev_info(&dev->dev,
            "USB scheduling: uframes_per_urb=%u num_urbs=%u total_uframes=%u rec_safety=%u\n",
            priv->rt_uframes_per_urb, priv->rt_num_urbs,
            priv->rt_total_uframes, priv->rt_rec_safety_offset);

        mutex_init(&priv->pcm_mutex);

        err = set_runtime_rate(priv, sample_rate);
        if (err) {
            dev_err(&dev->dev, "Failed to set initial rate %d: %d\n", sample_rate, err);
            goto error_free_card_direct;
        }

        usb_set_intfdata(intf, priv);

        pb_intf = usb_ifnum_to_if(dev, 1);
        rec_intf = usb_ifnum_to_if(dev, 2);

        if (!pb_intf || !rec_intf) {
            err = -ENODEV;
            goto error_free;
        }

        err = usb_driver_claim_interface(&snd_usb_motu_driver, pb_intf, priv);

        if (err)
            goto error_free;

        err = usb_driver_claim_interface(&snd_usb_motu_driver, rec_intf, priv);

        if (err)
            goto error_free;

        err = alloc_interrupt_urbs(priv, intf, interrupt_complete_urb);

        if (err)
            goto error_free;
        
        err = alloc_stream_urbs(priv, &priv->pb_stream, pb_intf,
            playback_complete_urb);

        if (err)
            goto error_free;
        
        err = alloc_stream_urbs(priv, &priv->rec_stream, rec_intf,
            capture_complete_urb);

        if (err)
            goto error_free;
        
        err = pcm_init(priv);

        if (err)
            goto error_free;
        
        spin_lock_init(&priv->rec_stream.lock);
        spin_lock_init(&priv->pb_stream.lock);
        spin_lock_init(&priv->wq_lock);

        priv->start_stop_wq = alloc_ordered_workqueue("snd_usb_motu_wq", WQ_HIGHPRI);

        if (!priv->start_stop_wq ) {
            err = -ENOMEM;
            goto error_free;
        }

        INIT_DELAYED_WORK(&priv->start_streams_work, start_streaming_endpoints);
        INIT_DELAYED_WORK(&priv->stop_streams_work, stop_streaming_endpoints);
        INIT_WORK(&priv->xrun_work, xrun_work_fn);

        strcpy(card->driver, "MOTU Driver");
        strcpy(card->shortname, "MOTU Pro Audio");
        strcpy(card->longname, "MOTU Pro Audio");
        
        motu_create_proc_entry(priv);
        
        err = snd_card_register(card);

        if (err)
            goto error_free;
        
        start_interrupt_urbs(priv);
    }

    dev_info(&dev->dev, "Probe Success!\n");
    return 0;

error_free:
    dev_info(&dev->dev, "Probe Error: Freeing Card\n");
    snd_card_free(priv->card);
    return err;
error_free_card_direct:
    snd_card_free(card);
    return err;
}

static void motu_usb_audio_disconnect(struct usb_interface *intf)
{
    struct usb_device *dev = interface_to_usbdev(intf);
    struct motu_usb_data *priv = usb_get_intfdata(intf);
    int ifnum = intf->altsetting->desc.bInterfaceNumber;

    dev_info(&dev->dev, "Disconnecting Interface: %d\n", ifnum);

    if ((ifnum == 0) && priv) {
        struct workqueue_struct *wq = priv->start_stop_wq;
        dev_info(&dev->dev, "Disconnect: Freeing Card\n");
        priv->disconnected = true;
        snd_card_disconnect(priv->card);
        spin_lock(&priv->wq_lock);
        priv->start_stop_wq = NULL;
        spin_unlock(&priv->wq_lock);
        cancel_delayed_work_sync(&priv->start_streams_work);
        cancel_delayed_work_sync(&priv->stop_streams_work);
        cancel_work_sync(&priv->xrun_work);
        destroy_workqueue(wq);
        atomic_set(&priv->streams_started, 0);
        free_interrupt_ubs(priv);
        free_stream_urbs(priv, &priv->rec_stream);
        free_stream_urbs(priv, &priv->pb_stream);
        snd_card_free_when_closed(priv->card);
    }
}

static const struct usb_device_id motu_usb_ids[] = {
    { USB_DEVICE(0x07fd, 0x0005) }, /* MOTU Pro Audio Devices */
    { }
};

MODULE_DEVICE_TABLE(usb, motu_usb_ids);

static struct usb_driver snd_usb_motu_driver = {
    .name = "snd-usb-motu",
    .id_table = motu_usb_ids,
    .probe = motu_usb_audio_probe,
    .disconnect = motu_usb_audio_disconnect,
};

module_usb_driver(snd_usb_motu_driver);