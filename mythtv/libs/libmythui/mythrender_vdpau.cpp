#include "math.h"

#include <QSize>

#include "mythverbose.h"
#include "mythrender_vdpau.h"

// NB this may be API dependant
#ifndef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
#define VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 (VdpVideoMixerFeature)11
#endif
#define NUM_SCALING_LEVELS 9

#define LOC      QString("VDPAU: ")
#define LOC_ERR  QString("VDPAU Error: ")
#define LOC_WARN QString("VDPAU Warning: ")

#define LOCK_RENDER QMutexLocker locker1(&m_render_lock);
#define LOCK_DECODE QMutexLocker locker2(&m_decode_lock);
#define LOCK_ALL    LOCK_RENDER; LOCK_DECODE;

#define INIT_ST \
  VdpStatus vdp_st; \
  bool ok = true;

#define CHECK_ST \
  ok &= (vdp_st == VDP_STATUS_OK); \
  if (!ok) { \
      VERBOSE(VB_GENERAL, LOC_ERR + QString("Error at %1:%2 (#%3, %4)") \
              .arg(__FILE__).arg( __LINE__).arg(vdp_st) \
              .arg(vdp_get_error_string(vdp_st))); \
  }

#define CHECK_STATUS(arg1) \
  if (m_preempted) \
  { \
      LOCK_ALL \
      Preempted(); \
  } \
  if (m_errored) \
      return arg1;

#define GET_PROC(FUNC_ID, PROC) \
    vdp_st = vdp_get_proc_address( \
        m_device, FUNC_ID, (void **)&PROC); \
    CHECK_ST

#define CREATE_CHECK(arg1, arg2) \
  if (ok) \
  { \
      ok = arg1; \
      if (!ok) \
          VERBOSE(VB_IMPORTANT, LOC_ERR + arg2); \
  }

#define CHECK_VIDEO_SURFACES(arg1) \
  if (m_reset_video_surfaces) \
      ResetVideoSurfaces(); \
  if (m_reset_video_surfaces) \
      return arg1;

static const VdpOutputSurfaceRenderBlendState vdpblend =
{
    VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
};

static const VdpOutputSurfaceRenderBlendState pipblend =
{
    VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
};

class VDPAUCSCMatrix
{
  public:
    VDPAUCSCMatrix(VdpColorStandard std = VDP_COLOR_STANDARD_ITUR_BT_601,
                   bool studio = false)
      : m_std(std), m_studio(studio)
    {
        m_procamp.struct_version = VDP_PROCAMP_VERSION;
        m_procamp.brightness     = 0.0f;
        m_procamp.contrast       = 1.0f;
        m_procamp.saturation     = 1.0f;
        m_procamp.hue            = 0.0f;
        memset(&m_csc, 0, sizeof(VdpCSCMatrix));
    }
    void SetBrightness(int val) { m_procamp.brightness = (val * 0.02f) - 1.0f; }
    void SetContrast(int val)   { m_procamp.contrast = (val * 0.02f);          }
    void SetColour(int val)     { m_procamp.saturation = (val * 0.02f);        }
    void SetHue(int val)
    {
        float new_val = (val * 0.062831853f);
        if (new_val > 3.14159265f)
            new_val -= 6.2831853f;
        m_procamp.hue = new_val;
    }

    bool ManualUpdate(void)
    {
        if (!m_studio)
            return false;

        static const float color_coeffs[][3] = {{ 0.299, 0.587, 0.114},
                                                { 0.2125, 0.7154, 0.0721}};
        int csp = (m_std == VDP_COLOR_STANDARD_ITUR_BT_601) ? 0 : 1;
        float uvcos = m_procamp.saturation * cos(m_procamp.hue);
        float uvsin = m_procamp.saturation * sin(m_procamp.hue);
        float Kr, Kg, Kb;
        int rgbmin = 16;
        int rgbr = 235-16;

        Kr = color_coeffs[csp][0];
        Kg = color_coeffs[csp][1];
        Kb = color_coeffs[csp][2];

        float uv_coeffs[3][2] = {
            { 0.000,                      (rgbr/112.0)*(1-Kr)       },
            {-(rgbr/112.0)*(1-Kb)*Kb/Kg, -(rgbr/112.0)*(1-Kr)*Kr/Kg },
            { (rgbr/112.0)*(1-Kb),        0.000                     }
        };

        for (int i = 0; i < 3; i++)
        {
            m_csc[i][3]  = m_procamp.brightness;
            m_csc[i][0]  = rgbr * m_procamp.contrast / 219;
            m_csc[i][3] += (-16 / 255.0) * m_csc[i][0];
            m_csc[i][1]  = uv_coeffs[i][0] * uvcos + uv_coeffs[i][1] * uvsin;
            m_csc[i][3] += (-128 / 255.0) * m_csc[i][1];
            m_csc[i][2]  = uv_coeffs[i][0] * uvsin + uv_coeffs[i][1] * uvcos;
            m_csc[i][3] += (-128 / 255.0) * m_csc[i][2];
            m_csc[i][3] += rgbmin / 255.0;
            m_csc[i][3] += 0.5 - m_procamp.contrast / 2.0;
        }
        return true;
    }

    VdpColorStandard m_std;
    bool             m_studio;
    VdpProcamp       m_procamp;
    VdpCSCMatrix     m_csc;
};

class VDPAUColor
{
  public:
    VDPAUColor(int color = 0x0) : m_color(color)
    {
        SetColor(m_color);
    }

    void SetColor(uint color)
    {
        m_color = color;
        m_vdp_color.red   = (float)((m_color & 0xFF000000) >> 24) / 255.0f;
        m_vdp_color.green = (float)((m_color & 0xFF0000) >> 16) / 255.0f;
        m_vdp_color.blue  = (float)((m_color & 0xFF00) >> 8)/ 255.0f;
        m_vdp_color.alpha = (float)( m_color & 0xFF) / 255.0f;
    }

    int      m_color;
    VdpColor m_vdp_color;
};

class VDPAULayer
{
  public:
    VDPAULayer() {}
    VDPAULayer(uint surface, const QRect *src, const QRect *dst)
    {
        if (src)
        {
            m_src.x0 = src->left();
            m_src.y0 = src->top();
            m_src.x1 = src->left() + src->width();
            m_src.y1 = src->top() +  src->height();
        }
        if (dst)
        {
            m_dst.x0 = dst->left();
            m_dst.y0 = dst->top();
            m_dst.x1 = dst->left() + dst->width();
            m_dst.y1 = dst->top() +  dst->height();
        }
        m_layer.struct_version   = VDP_LAYER_VERSION;
        m_layer.source_surface   = surface;
        m_layer.source_rect      = src ? &m_src : NULL;
        m_layer.destination_rect = dst ? &m_dst : NULL;
    }

    VdpLayer m_layer;
    VdpRect  m_src;
    VdpRect  m_dst;
};

class VDPAUResource
{
  public:
    VDPAUResource() {}
    VDPAUResource(uint id, QSize size) : m_id(id), m_size(size) { }

    uint  m_id;
    QSize m_size;
};

class VDPAUOutputSurface : public VDPAUResource
{
  public:
    VDPAUOutputSurface() {}
    VDPAUOutputSurface(uint id, QSize size, VdpRGBAFormat fmt)
      : VDPAUResource(id, size), m_fmt(fmt) { }

    VdpRGBAFormat m_fmt;
};

class VDPAUVideoSurface : public VDPAUResource
{
  public:
    VDPAUVideoSurface() {}
    VDPAUVideoSurface(uint id, QSize size, VdpChromaType type)
      : VDPAUResource(id, size), m_type(type), m_needs_reset(false)
    {
        m_owner = pthread_self();
        memset(&m_render, 0, sizeof(struct vdpau_render_state));
        m_render.surface = m_id;
    }
    void SetID(uint id)
    {
        // NB render data needs to be updated as well but we cannot do that here
        // as, for example, the new id's of reference frames are not known
        m_id = id;
        m_render.surface = m_id;
    }

    VdpChromaType      m_type;
    vdpau_render_state m_render;
    bool               m_needs_reset;
    pthread_t          m_owner;
};

class VDPAUBitmapSurface : public VDPAUResource
{
  public:
    VDPAUBitmapSurface() {}
    VDPAUBitmapSurface(uint id, QSize size, VdpRGBAFormat fmt)
      : VDPAUResource(id, size), m_fmt(fmt) { }

    VdpRGBAFormat m_fmt;
};

class VDPAUDecoder : public VDPAUResource
{
  public:
    VDPAUDecoder() {}
    VDPAUDecoder(uint id, QSize size, VdpDecoderProfile profile, uint refs)
      : VDPAUResource(id, size), m_profile(profile), m_max_refs(refs) { }

    VdpDecoderProfile m_profile;
    uint              m_max_refs;
};

class VDPAUVideoMixer : public VDPAUResource
{
  public:
    VDPAUVideoMixer() {}
    VDPAUVideoMixer(uint id, QSize size, uint layers, uint features,
                    VdpChromaType type)
     : VDPAUResource(id, size), m_layers(layers), m_features(features),
      m_type(type), m_csc(NULL), m_noise_reduction(NULL), m_sharpness(NULL),
      m_skip_chroma(NULL), m_background(NULL) { }
   ~VDPAUVideoMixer()
    {
        if (m_csc)
            delete m_csc;
        if (m_noise_reduction)
            delete m_noise_reduction;
        if (m_sharpness)
            delete m_sharpness;
        if (m_skip_chroma)
            delete m_skip_chroma;
        if (m_background)
            delete m_background;
    }

    uint            m_layers;
    uint            m_features;
    VdpChromaType   m_type;
    VDPAUCSCMatrix *m_csc;
    float          *m_noise_reduction;
    float          *m_sharpness;
    uint8_t        *m_skip_chroma;
    VDPAUColor     *m_background;
};

static void vdpau_preemption_callback(VdpDevice device, void *myth_render)
{
    (void)device;
    VERBOSE(VB_IMPORTANT, LOC_WARN + "Display pre-empted.");
    MythRenderVDPAU *render = (MythRenderVDPAU*)myth_render;
    if (render)
        render->SetPreempted();
}

MythRenderVDPAU::MythRenderVDPAU()
  : m_preempted(false), m_recreating(false),
    m_recreated(false), m_reset_video_surfaces(false),
    m_render_lock(QMutex::Recursive), m_decode_lock(QMutex::Recursive),
    m_display(NULL), m_window(0), m_device(0), m_surface(0),
    m_flipQueue(0),  m_flipTarget(0), m_flipReady(false), m_colorKey(0),
    m_best_scaling(0), vdp_get_proc_address(NULL), vdp_get_error_string(NULL)
{
    LOCK_ALL
    ResetProcs();
    memset(&m_rect, 0, sizeof(VdpRect));
}

MythRenderVDPAU::~MythRenderVDPAU(void)
{
    LOCK_ALL
    Destroy();
}

bool MythRenderVDPAU::Create(const QSize &size, WId window, uint colorkey)
{
    LOCK_ALL

    m_size    = size;
    m_rect.x0 = 0;
    m_rect.y0 = 0;
    m_rect.x1 = size.width();
    m_rect.y1 = size.height();
    m_display = OpenMythXDisplay();
    m_window  = window;

    bool ok = true;

    CREATE_CHECK(!m_size.isEmpty(), "Invalid size")
    CREATE_CHECK(m_display != NULL, "Invalid display")
    CREATE_CHECK(window >0, "Invalid window")
    CREATE_CHECK(m_display->CreateGC(m_window), "No GC")
    CREATE_CHECK(CreateDevice(), "No VDPAU device")
    CREATE_CHECK(GetProcs(), "No VDPAU procedures")
    CREATE_CHECK(CreatePresentationQueue(), "No presentation queue")
    CREATE_CHECK(CreatePresentationSurfaces(), "No presentation surfaces")
    CREATE_CHECK(SetColorKey(colorkey), "No colorkey")
    CREATE_CHECK(RegisterCallback(), "No callback")
    CREATE_CHECK(GetBestScaling(), "")

    if (ok)
    {
        VERBOSE(VB_GENERAL, LOC + QString("Created VDPAU render device %1x%2")
           .arg(size.width()).arg(size.height()));
        return ok;
    }

    VERBOSE(VB_IMPORTANT, "Failed to create VDPAU render device.");
    return ok;
}

void MythRenderVDPAU::RenderLock(bool lock)
{
    lock ? m_render_lock.lock() : m_render_lock.unlock();
}

void MythRenderVDPAU::DecodeLock(bool lock)
{
    lock ? m_decode_lock.lock() : m_decode_lock.unlock();
}
    
bool MythRenderVDPAU::WasPreempted(void)
{
    // tells the UI Painter to refresh its cache
    if (m_recreated)
    {
        m_recreated = false;
        return true;
    }
    return false;
}

bool MythRenderVDPAU::SetColorKey(uint colorkey)
{
    LOCK_RENDER
    CHECK_STATUS(false)
    INIT_ST

    m_colorKey = colorkey;
    if (m_display && (m_display->GetDepth() < 24))
        m_colorKey = 0x0;

    VDPAUColor color((colorkey << 8) + 0xFF);
    vdp_st = vdp_presentation_queue_set_background_color(m_flipQueue,
                                                        &(color.m_vdp_color));
    CHECK_ST

    VERBOSE(VB_PLAYBACK, LOC + QString("Set colorkey to 0x%1")
            .arg(m_colorKey, 0, 16));
    return ok;
}

void MythRenderVDPAU::WaitForFlip(void)
{
    if (!m_flipReady)
        return;

    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    VdpTime dummy = 0;
    usleep(2000);
    VdpOutputSurface surface = m_outputSurfaces[m_surfaces[m_surface]].m_id;
    vdp_st = vdp_presentation_queue_block_until_surface_idle(
                m_flipQueue, surface, &dummy);
    CHECK_ST
}

void MythRenderVDPAU::Flip(int delay)
{
    if (!m_flipReady)
        return;

    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    VdpTime now = 0;

    if (delay > 0 && vdp_presentation_queue_get_time)
    {
        vdp_st = vdp_presentation_queue_get_time(m_flipQueue, &now);
        CHECK_ST
        now += delay * 1000;
    }

    VdpOutputSurface surface = m_outputSurfaces[m_surfaces[m_surface]].m_id;
    vdp_st = vdp_presentation_queue_display(m_flipQueue, surface, m_rect.x1, m_rect.y1, now);
    CHECK_ST

    m_surface++;
    if (m_surface >= (uint)m_surfaces.size())
        m_surface = 0;
}

uint MythRenderVDPAU::CreateOutputSurface(const QSize &size, VdpRGBAFormat fmt,
                                          uint existing)
{
    LOCK_RENDER
    CHECK_STATUS(0)
    INIT_ST

    if ((existing && !m_outputSurfaces.contains(existing)) || size.isEmpty())
        return 0;

    VdpOutputSurface tmp;
    vdp_st = vdp_output_surface_create(m_device, fmt, size.width(),
                                       size.height(), &tmp);
    CHECK_ST

    if (!ok || !tmp)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
            QString("Failed to create output surface."));
        return 0;
    }

    if (existing)
    {
        m_outputSurfaces[existing].m_id = tmp;
        return existing;
    }

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_outputSurfaces.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_outputSurfaces.insert(id, VDPAUOutputSurface(tmp, size, fmt));
    id_lock.unlock();

    DrawBitmap(0, id, NULL, NULL);
    return id;
}

uint MythRenderVDPAU::CreateVideoSurface(const QSize &size, VdpChromaType type,
                                         uint existing)
{
    LOCK_RENDER
    CHECK_STATUS(0)
    INIT_ST

    if ((existing && !m_videoSurfaces.contains(existing)) || size.isEmpty())
        return 0;

    VdpOutputSurface tmp;
    vdp_st = vdp_video_surface_create(m_device, type, size.width(),
                                      size.height(), &tmp);
    CHECK_ST

    if (!ok || !tmp)
    {
        VERBOSE(VB_PLAYBACK, LOC_ERR +
            QString("Failed to create video surface."));
        return 0;
    }

    if (existing)
    {
        m_videoSurfaces[existing].SetID(tmp);
        m_videoSurfaceHash[tmp] = existing;
        return existing;
    }

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_videoSurfaces.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_videoSurfaces.insert(id, VDPAUVideoSurface(tmp, size, type));
    m_videoSurfaceHash[tmp] = id;
    id_lock.unlock();

    return id;
}

uint MythRenderVDPAU::CreateBitmapSurface(const QSize &size, VdpRGBAFormat fmt,
                                          uint existing)
{
    LOCK_RENDER
    CHECK_STATUS(0)
    INIT_ST

    if ((existing && !m_bitmapSurfaces.contains(existing)) || size.isEmpty())
        return 0;

    VdpBitmapSurface tmp;
    vdp_st = vdp_bitmap_surface_create(m_device, fmt, size.width(),
                                       size.height(), true, &tmp);
    CHECK_ST

    if (!ok || !tmp)
    {
        VERBOSE(VB_GENERAL, LOC_ERR +
            QString("Failed to create bitmap surface."));
        return 0;
    }

    if (existing)
    {
        m_bitmapSurfaces[existing].m_id = tmp;
        return existing;
    }

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_bitmapSurfaces.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_bitmapSurfaces.insert(id, VDPAUBitmapSurface(tmp, size, fmt));
    id_lock.unlock();

    return id;
}

uint MythRenderVDPAU::CreateDecoder(const QSize &size,
                                    VdpDecoderProfile profile,
                                    uint references, uint existing)
{
    LOCK_DECODE
    CHECK_STATUS(0)
    INIT_ST

    if ((existing && !m_decoders.contains(existing)) || size.isEmpty() ||
        references < 1)
        return 0;

    VdpDecoder tmp;
    vdp_st = vdp_decoder_create(m_device, profile, size.width(),
                                size.height(), references, &tmp);
    CHECK_ST

    if (!ok || !tmp)
    {
        VERBOSE(VB_PLAYBACK, LOC_ERR +
            QString("Failed to create decoder."));
        return 0;
    }

    if (existing)
    {
        m_decoders[existing].m_id = tmp;
        return existing;
    }

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_decoders.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_decoders.insert(id, VDPAUDecoder(tmp, size, profile, references));
    id_lock.unlock();

    return id;
}

uint MythRenderVDPAU::CreateVideoMixer(const QSize &size, uint layers,
                                       uint features, VdpChromaType type,
                                       uint existing)
{
    LOCK_RENDER
    CHECK_STATUS(0)
    INIT_ST

    if ((existing && !m_videoMixers.contains(existing)) || size.isEmpty())
        return 0;

    VdpVideoMixer tmp;
    uint width  = size.width();
    uint height = size.height();

    VdpVideoMixerParameter parameters[] = {
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE,
        VDP_VIDEO_MIXER_PARAMETER_LAYERS,
    };

    void const * parameter_values[] = { &width, &height, &type, &layers};

    int count = 0;
    VdpVideoMixerFeature feat[6];
    VdpBool enable = true;
    const VdpBool enables[6] = { enable, enable, enable, enable, enable, enable };

    bool temporal = (features & kVDPFeatTemporal) ||
                    (features & kVDPFeatSpatial);
    if (temporal)
    {
        feat[count] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
        count++;
    }

    if (features & kVDPFeatSpatial)
    {
        feat[count] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
        count++;
    }

    if ((features & kVDPFeatIVTC) && temporal)
    {
        feat[count] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
        count++;
    }

    if (features & kVDPFeatDenoise)
    {
        feat[count] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
        count++;
    }

    if (features & kVDPFeatSharpness)
    {
        feat[count] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
        count++;
    }

    if (features & kVDPFeatHQScaling)
    {
        if (m_best_scaling)
        {
            feat[count] = m_best_scaling;
            count++;
            VERBOSE(VB_PLAYBACK, LOC +
                    QString("Enabling high quality scaling."));
        }
        else
            VERBOSE(VB_PLAYBACK, LOC + "High quality scaling not available");
    }

    vdp_st = vdp_video_mixer_create(m_device, count, count ? feat : NULL,
                                    4, parameters, parameter_values, &tmp);
    CHECK_ST

    if (!ok || !tmp)
    {
        VERBOSE(VB_PLAYBACK, LOC_ERR +
            QString("Failed to create video mixer."));
        return 0;
    }

    vdp_st = vdp_video_mixer_set_feature_enables(
        tmp, count, count ? feat : NULL, count ? enables : NULL);
    CHECK_ST

    if (!ok)
        VERBOSE(VB_PLAYBACK, LOC_ERR +
            QString("WARNING: Failed to enable video mixer features."));

    if (existing)
    {
        m_videoMixers[existing].m_id       = tmp;
        m_videoMixers[existing].m_features = features;
        m_videoMixers[existing].m_type     = type;
        m_videoMixers[existing].m_size     = size;

        if (m_videoMixers[existing].m_csc)
            SetMixerAttribute(existing, kVDPAttribColour,
                              m_videoMixers[existing].m_csc->m_procamp.saturation);
        if (m_videoMixers[existing].m_noise_reduction)
            SetMixerAttribute(existing, kVDPAttribNoiseReduction,
                            *(m_videoMixers[existing].m_noise_reduction));
        if (m_videoMixers[existing].m_sharpness)
            SetMixerAttribute(existing, kVDPAttribSharpness,
                            *(m_videoMixers[existing].m_sharpness));
        if (m_videoMixers[existing].m_skip_chroma)
            SetMixerAttribute(existing, kVDPAttribSkipChroma,
                            *(m_videoMixers[existing].m_skip_chroma));
        if (m_videoMixers[existing].m_background)
            SetMixerAttribute(existing, kVDPAttribBackground,
                              m_videoMixers[existing].m_background->m_color);
        return existing;
    }

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_videoMixers.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_videoMixers.insert(id,
        VDPAUVideoMixer(tmp, size, layers, features, type));
    id_lock.unlock();

    return id;
}

uint MythRenderVDPAU::CreateLayer(uint surface, const QRect *src,
                                  const QRect *dst)
{
    LOCK_RENDER
    CHECK_STATUS(0)

    if (!m_outputSurfaces.contains(surface))
        return 0;

    static uint32_t next_id = 1;
    static QMutex id_lock(QMutex::Recursive);

    id_lock.lock();
    while (m_layers.contains(next_id))
        if ((++next_id) == 0)
            next_id = 1;

    uint id = next_id;
    m_layers.insert(id, VDPAULayer(m_outputSurfaces[surface].m_id, src, dst));
    id_lock.unlock();

    return id;
}

void MythRenderVDPAU::DestroyOutputSurface(uint id)
{
    if (!vdp_output_surface_destroy)
        return;

    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    if (!m_outputSurfaces.contains(id))
        return;

    vdp_st = vdp_output_surface_destroy(m_outputSurfaces[id].m_id);
    CHECK_ST
    m_outputSurfaces.remove(id);
}

void MythRenderVDPAU::DestroyVideoSurface(uint id)
{
    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    if (!m_videoSurfaces.contains(id))
        return;

    vdp_st = vdp_video_surface_destroy(m_videoSurfaces[id].m_id);
    CHECK_ST
    m_videoSurfaceHash.remove(m_videoSurfaces[id].m_id);
    m_videoSurfaces.remove(id);
}

void MythRenderVDPAU::DestroyBitmapSurface(uint id)
{
    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    if (!m_bitmapSurfaces.contains(id))
        return;

    vdp_st = vdp_bitmap_surface_destroy(m_bitmapSurfaces[id].m_id);
    CHECK_ST
    m_bitmapSurfaces.remove(id);
}

void MythRenderVDPAU::DestroyDecoder(uint id)
{
    LOCK_DECODE
    CHECK_STATUS()
    INIT_ST

    if (!m_decoders.contains(id))
        return;

    vdp_st = vdp_decoder_destroy(m_decoders[id].m_id);
    CHECK_ST
    m_decoders.remove(id);
}

void MythRenderVDPAU::DestroyVideoMixer(uint id)
{
    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    if (!m_videoMixers.contains(id))
        return;

    vdp_st = vdp_video_mixer_destroy(m_videoMixers[id].m_id);
    CHECK_ST
    m_videoMixers.remove(id);
}

void MythRenderVDPAU::DestroyLayer(uint id)
{
    LOCK_RENDER
    CHECK_STATUS()

    if (!m_layers.contains(id))
        return;

    m_layers.remove(id);
}
    
bool MythRenderVDPAU::MixAndRend(uint id, VdpVideoMixerPictureStructure field,
                                 uint vid_surface, uint out_surface,
                                 const QVector<uint>* refs, bool top,
                                 QRect src, const QSize &dst,
                                 QRect dst_vid, uint layer1, uint layer2)
{
    CHECK_VIDEO_SURFACES(true)
    LOCK_RENDER
    CHECK_STATUS(false)
    INIT_ST

    if (!out_surface)
        out_surface = m_surfaces[m_surface];

    if (!m_videoMixers.contains(id) ||
        !m_videoSurfaces.contains(vid_surface)||
        !m_outputSurfaces.contains(out_surface))
        return false;

    if (dst_vid.top() < 0 && dst_vid.height() > 0)
    {
        float yscale = (float)src.height() /
                       (float)dst_vid.height();
        int tmp = src.top() -
                  (int)((float)dst_vid.top() * yscale);
        src.setTop(std::max(0, tmp));
        dst_vid.setTop(0);
    }

    if (dst_vid.left() < 0 && dst_vid.width() > 0)
    {
        float xscale = (float)src.width() /
                       (float)dst_vid.width();
        int tmp = src.left() -
                  (int)((float)dst_vid.left() * xscale);
        src.setLeft(std::max(0, tmp));
        dst_vid.setLeft(0);
    }

    VdpRect outRect, srcRect, outRectVid;

    outRect.x0    = 0;
    outRect.y0    = 0;
    outRect.x1    = dst.width();
    outRect.y1    = dst.height();
    srcRect.x0    = src.left();
    srcRect.y0    = src.top();
    srcRect.x1    = src.left() + src.width();
    srcRect.y1    = src.top() +  src.height();
    outRectVid.x0 = dst_vid.left();
    outRectVid.y0 = dst_vid.top();
    outRectVid.x1 = dst_vid.left() + dst_vid.width();
    outRectVid.y1 = dst_vid.top() +  dst_vid.height();

    VdpVideoSurface past_surfaces[2]   = { VDP_INVALID_HANDLE,
                                           VDP_INVALID_HANDLE };
    VdpVideoSurface future_surfaces[1] = { VDP_INVALID_HANDLE };
    VdpVideoSurface vid_surf = m_videoSurfaces[vid_surface].m_id;
    bool deint = false;

    if (refs && refs->size() == NUM_REFERENCE_FRAMES)
    {
        deint = true;
        VdpVideoSurface act_refs[NUM_REFERENCE_FRAMES];
        for (int i = 0; i < NUM_REFERENCE_FRAMES; i++)
        {
            if (m_videoSurfaces.contains(refs->value(i)))
                act_refs[i] = m_videoSurfaces[refs->value(i)].m_id;
            else
                act_refs[i] = VDP_INVALID_HANDLE;
        }

        vid_surf = act_refs[1];

        if (top)
        {
            future_surfaces[0] = act_refs[1];
            past_surfaces[0]   = act_refs[0];
            past_surfaces[1]   = act_refs[0];
        }
        else
        {
            future_surfaces[0] = act_refs[2];
            past_surfaces[0]   = act_refs[1];
            past_surfaces[1]   = act_refs[0];
        } 
    }

    uint num_layers = 0;
    VdpLayer layers[2];
    if (m_layers.contains(layer1))
    {
        memcpy(&(layers[num_layers]), &(m_layers[layer1].m_layer), sizeof(VdpLayer));
        num_layers++;
    }
    if (m_layers.contains(layer2))
    {
        memcpy(&(layers[num_layers]), &(m_layers[layer2].m_layer), sizeof(VdpLayer));
        num_layers++;
    }

    VdpOutputSurface surf = m_outputSurfaces[out_surface].m_id;
    vdp_st = vdp_video_mixer_render(m_videoMixers[id].m_id, VDP_INVALID_HANDLE,
                                    NULL, field, deint ? 2 : 0,
                                    deint ? past_surfaces : NULL,
                                    vid_surf, deint ? 1 : 0,
                                    deint ? future_surfaces : NULL,
                                    &srcRect, surf, &outRect, &outRectVid,
                                    num_layers, num_layers ? layers : NULL);
    CHECK_ST
    return ok;
}

bool MythRenderVDPAU::SetDeinterlacing(uint id, uint deinterlacers)
{
    LOCK_RENDER
    CHECK_STATUS(false)

    if (!m_videoMixers.contains(id))
        return false;

    static const uint all_deints = kVDPFeatTemporal & kVDPFeatSpatial;
    uint current = m_videoMixers[id].m_features;
    uint deints  = current & all_deints;

    if (deints == deinterlacers)
        return true;

    uint newfeats = (current & ~all_deints) + deinterlacers;
    return ChangeVideoMixerFeatures(id, newfeats);
}

bool MythRenderVDPAU::ChangeVideoMixerFeatures(uint id, uint features)
{
    LOCK_RENDER
    CHECK_STATUS(false)

    if (!m_videoMixers.contains(id))
        return false;

    INIT_ST
    vdp_st = vdp_video_mixer_destroy(m_videoMixers[id].m_id);
    CHECK_ST
    return (id == CreateVideoMixer(m_videoMixers[id].m_size,
                                   m_videoMixers[id].m_layers, features,
                                   m_videoMixers[id].m_type, id));
}

bool MythRenderVDPAU::SetMixerAttribute(uint id, uint attrib, int value)
{
    LOCK_RENDER
    CHECK_STATUS(false);
    INIT_ST

    if (!m_videoMixers.contains(id) || attrib > kVDPAttribCSCEnd)
        return false;

    if (attrib == kVDPAttribSkipChroma)
    {
        if (!m_videoMixers[id].m_skip_chroma)
            m_videoMixers[id].m_skip_chroma = new uint8_t();
        *(m_videoMixers[id].m_skip_chroma) = value;
        VdpVideoMixerAttribute attr =
            { VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE };
        void const * val = { &(m_videoMixers[id].m_skip_chroma) };
        return SetMixerAttribute(id, &attr, &val);
    }

    if (attrib == kVDPAttribBackground)
    {
        if (!m_videoMixers[id].m_background)
            m_videoMixers[id].m_background = new VDPAUColor();
        m_videoMixers[id].m_background->SetColor(value);
        VdpVideoMixerAttribute attr =
            { VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR };
        void const * val = { &(m_videoMixers[id].m_background->m_vdp_color) };
        return SetMixerAttribute(id, &attr, &val);
    }

    if (!m_videoMixers[id].m_csc)
    {
        m_videoMixers[id].m_csc = new VDPAUCSCMatrix();
        if (!m_videoMixers[id].m_csc)
            return false;
    }

    if (attrib == kVDPAttribColorStandard)
        m_videoMixers[id].m_csc->m_std = value;
    else if (attrib == kVDPAttribStudioLevels)
        m_videoMixers[id].m_csc->m_studio = value;
    else if (attrib == kVDPAttribHue)
        m_videoMixers[id].m_csc->SetHue(value);
    else if (attrib == kVDPAttribContrast)
        m_videoMixers[id].m_csc->SetContrast(value);
    else if (attrib == kVDPAttribColour)
        m_videoMixers[id].m_csc->SetColour(value);
    else if (attrib == kVDPAttribBrightness)
        m_videoMixers[id].m_csc->SetBrightness(value);
    else
        return false;

    if (!m_videoMixers[id].m_csc->ManualUpdate())
    {
        vdp_st = vdp_generate_csc_matrix(&(m_videoMixers[id].m_csc->m_procamp),
                                         m_videoMixers[id].m_csc->m_std,
                                         &(m_videoMixers[id].m_csc->m_csc));
        CHECK_ST
    }

    VdpVideoMixerAttribute attr = { VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX };
    void const * val = { &(m_videoMixers[id].m_csc->m_csc) };
    return SetMixerAttribute(id, &attr, &val);
}

bool MythRenderVDPAU::SetMixerAttribute(uint id, uint attrib, float value)
{
    LOCK_RENDER
    CHECK_STATUS(false);

    if (!m_videoMixers.contains(id) || attrib < kVDPAttribFiltersStart)
        return false;

    VdpVideoMixerAttribute attr;
    void const * val = { &value };

    if (attrib == kVDPAttribNoiseReduction)
    {
        if (!m_videoMixers[id].m_noise_reduction)
            m_videoMixers[id].m_noise_reduction = new float();
        *(m_videoMixers[id].m_noise_reduction) = value;
        attr = VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL;
    }
    else if (attrib == kVDPAttribSharpness)
    {
        if (!m_videoMixers[id].m_sharpness)
            m_videoMixers[id].m_sharpness = new float();
        *(m_videoMixers[id].m_sharpness) = value;
        attr = VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL;
    }
    else
        return false;

    return SetMixerAttribute(id, &attr, &val);
}

bool MythRenderVDPAU::UploadBitmap(uint id, void* const plane[1], uint32_t pitch[1])
{
    LOCK_RENDER
    CHECK_STATUS(false)
    INIT_ST

    vdp_st = vdp_bitmap_surface_put_bits_native(
                m_bitmapSurfaces[id].m_id, plane, pitch, NULL);
    CHECK_ST

    return ok;
}

bool MythRenderVDPAU::UploadMythImage(uint id, MythImage *image)
{
    if (!image)
        return false;

    void    *plane[1] = { image->bits() };
    uint32_t pitch[1] = { image->bytesPerLine() };
    return UploadBitmap(id, plane, pitch);
}

bool MythRenderVDPAU::UploadYUVFrame(uint id, void* const planes[3],
                                     uint32_t pitches[3])
{
    CHECK_VIDEO_SURFACES(false)
    LOCK_RENDER
    CHECK_STATUS(false)
    INIT_ST

    if (!m_videoSurfaces.contains(id))
        return false;

    vdp_st = vdp_video_surface_put_bits_y_cb_cr(m_videoSurfaces[id].m_id,
                                                VDP_YCBCR_FORMAT_YV12,
                                                planes, pitches);
    CHECK_ST;
    return ok;
}

bool MythRenderVDPAU::DrawBitmap(uint id, uint target,
                                 const QRect *src, const QRect *dst,
                                 int alpha, int red, int green, int blue,
                                 bool blend)
{
    LOCK_RENDER
    CHECK_STATUS(false)
    INIT_ST

    if (!target)
        target = m_surfaces[m_surface];

    if (!m_outputSurfaces.contains(target))
        return false;

    VdpRect vdest, vsrc;
    if (dst)
    {
        int width  = dst->width();
        int height = dst->height();

        if (src)
        {
            width = std::min(src->width(), width);
            height = std::min(src->height(), height);
        }

        vdest.x0 = dst->x();
        vdest.y0 = dst->y();
        vdest.x1 = dst->x() + width;
        vdest.y1 = dst->y() + height;

        if (vdest.x0 < 0)
            vdest.x0 = 0;
        if (vdest.y0 < 0)
            vdest.y0 = 0;
    }

    if (src)
    {
        vsrc.x0 = src->x();
        vsrc.y0 = src->y();
        vsrc.x1 = src->x() + src->width();
        vsrc.y1 = src->y() + src->height();
    }

    VdpColor color;
    bool nullblend = (red == 0 && green == 0 && blue == 0 && alpha == 0);
    if (!nullblend)
    {
        color.red   = red   / 255.0f;
        color.green = green / 255.0f;
        color.blue  = blue  / 255.0f;
        color.alpha = alpha / 255.0f;
    }

    const VdpOutputSurfaceRenderBlendState *bs =
        nullblend ? NULL :(blend ? &pipblend : &vdpblend);

    uint bitmap = VDP_INVALID_HANDLE;
    if (id && m_bitmapSurfaces.contains(id))
        bitmap = m_bitmapSurfaces[id].m_id;

    vdp_st = vdp_output_surface_render_bitmap_surface(
                m_outputSurfaces[target].m_id,
                dst ? &vdest : NULL, bitmap, src ? &vsrc  : NULL,
                alpha >= 0 ? &color : NULL, bs,
                VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    CHECK_ST
    return ok;
}

void* MythRenderVDPAU::GetRender(uint id)
{
    LOCK_RENDER
    CHECK_STATUS(NULL)

    if (!m_videoSurfaces.contains(id))
        return NULL;

    return &(m_videoSurfaces[id].m_render);
}

uint MythRenderVDPAU::GetSurfaceOwner(VdpVideoSurface surface)
{
    LOCK_RENDER
    CHECK_STATUS(NULL)

    if (!m_videoSurfaceHash.contains(surface))
        return 0;

    return m_videoSurfaceHash[surface];
}

void MythRenderVDPAU::ClearVideoSurface(uint id)
{
    CHECK_VIDEO_SURFACES()
    LOCK_RENDER
    CHECK_STATUS()
    INIT_ST

    if (!m_videoSurfaces.contains(id))
        return;

    uint width  = m_videoSurfaces[id].m_size.width();
    uint height = m_videoSurfaces[id].m_size.height();
    unsigned char *tmp = new unsigned char[(width * height * 3)>>1];

    if (!tmp)
        return;

    bzero(tmp, width * height);
    memset(tmp + (width * height), 127, (width * height)>>1);
    uint32_t pitches[3] = {width, width, width>>1};
    void* const planes[3] = {tmp, tmp + (width * height), tmp + (width * height)};
    vdp_st = vdp_video_surface_put_bits_y_cb_cr(m_videoSurfaces[id].m_id,
                                                VDP_YCBCR_FORMAT_YV12,
                                                planes, pitches);
    CHECK_ST
    delete [] tmp;
}

void MythRenderVDPAU::ChangeVideoSurfaceOwner(uint id)
{
    LOCK_ALL
    if (!m_videoSurfaces.contains(id))
        return;

    m_videoSurfaces[id].m_owner = pthread_self();
}

void MythRenderVDPAU::Decode(uint id, struct vdpau_render_state *render)
{
    CHECK_VIDEO_SURFACES()
    LOCK_DECODE
    CHECK_STATUS()
    INIT_ST

    if (!m_decoders.contains(id))
        return;

    vdp_st = vdp_decoder_render(m_decoders[id].m_id, render->surface,
                               (VdpPictureInfo const *)&(render->info),
                                render->bitstream_buffers_used,
                                render->bitstream_buffers);
    CHECK_ST
}

static const char* dummy_get_error_string(VdpStatus status)
{
    static const char dummy[] = "Unknown";
    return &dummy[0];
}

bool MythRenderVDPAU::CreateDevice(void)
{
    if (!m_display)
        return false;

    INIT_ST
    vdp_get_error_string = &dummy_get_error_string;
    XLOCK(m_display, vdp_st = vdp_device_create_x11(m_display->GetDisplay(),
                                  m_display->GetScreen(),
                                  &m_device, &vdp_get_proc_address));
    CHECK_ST

    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create VDPAU device.");
        return false;
    }

    vdp_st = vdp_get_proc_address(m_device, VDP_FUNC_ID_GET_ERROR_STRING,
                                 (void **)&vdp_get_error_string);
    ok &= (vdp_st == VDP_STATUS_OK);
    if (!ok)
    {
        vdp_get_error_string = &dummy_get_error_string;
        ok = true;
    }

    return ok;
}

bool MythRenderVDPAU::GetProcs(void)
{
    INIT_ST
    GET_PROC(VDP_FUNC_ID_DEVICE_DESTROY, vdp_device_destroy);
    GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_CREATE,  vdp_video_surface_create);
    GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, vdp_video_surface_destroy);
    GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
        vdp_video_surface_put_bits_y_cb_cr);
    GET_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE,  vdp_output_surface_create);
    GET_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY, vdp_output_surface_destroy);
    GET_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE,
        vdp_output_surface_render_bitmap_surface);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_CREATE, vdp_video_mixer_create);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES,
        vdp_video_mixer_set_feature_enables);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_DESTROY, vdp_video_mixer_destroy);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_RENDER, vdp_video_mixer_render);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES,
        vdp_video_mixer_set_attribute_values);
    GET_PROC(VDP_FUNC_ID_GENERATE_CSC_MATRIX, vdp_generate_csc_matrix);
    GET_PROC(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
        vdp_video_mixer_query_feature_support)
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY,
        vdp_presentation_queue_target_destroy);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE,
        vdp_presentation_queue_create);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY,
        vdp_presentation_queue_destroy);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY,
        vdp_presentation_queue_display);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
        vdp_presentation_queue_block_until_surface_idle);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
        vdp_presentation_queue_target_create_x11);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME,
        vdp_presentation_queue_get_time);
    GET_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR,
        vdp_presentation_queue_set_background_color);
    GET_PROC(VDP_FUNC_ID_DECODER_CREATE,  vdp_decoder_create);
    GET_PROC(VDP_FUNC_ID_DECODER_DESTROY, vdp_decoder_destroy);
    GET_PROC(VDP_FUNC_ID_DECODER_RENDER,  vdp_decoder_render);
    GET_PROC(VDP_FUNC_ID_BITMAP_SURFACE_CREATE, vdp_bitmap_surface_create);
    GET_PROC(VDP_FUNC_ID_BITMAP_SURFACE_DESTROY,vdp_bitmap_surface_destroy);
    GET_PROC(VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE,
        vdp_bitmap_surface_put_bits_native);
    GET_PROC(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER,
        vdp_preemption_callback_register);

    vdp_st = vdp_get_proc_address(
        m_device, VDP_FUNC_ID_GET_API_VERSION,
        (void **)&vdp_get_api_version
    );

    vdp_st = vdp_get_proc_address(
        m_device, VDP_FUNC_ID_GET_INFORMATION_STRING,
        (void **)&vdp_get_information_string
    );

    static bool debugged = false;

    if (!debugged)
    {
        debugged = true;
        if (vdp_get_api_version)
        {
            uint version;
            vdp_get_api_version(&version);
            VERBOSE(VB_GENERAL, LOC + QString("Version %1").arg(version));
        }
        if (vdp_get_information_string)
        {
            const char * info;
            vdp_get_information_string(&info);
            VERBOSE(VB_GENERAL, LOC + QString("Information %2").arg(info));
        }
    }

    return ok;
}

bool MythRenderVDPAU::CreatePresentationQueue(void)
{
    MythXLocker locker(m_display);
    m_surface = 0;
    INIT_ST
    vdp_st = vdp_presentation_queue_target_create_x11(m_device, m_window,
                                                     &m_flipTarget);
    CHECK_ST
    if (!ok)
        return false;

    vdp_st = vdp_presentation_queue_create(m_device, m_flipTarget,
                                          &m_flipQueue);
    CHECK_ST
    return ok;
}

bool MythRenderVDPAU::CreatePresentationSurfaces(void)
{
    int num = (m_master == kMasterUI) ?
               MIN_OUTPUT_SURFACES : MAX_OUTPUT_SURFACES;
    bool ok = true;

    for (int i = 0; i < num; i++)
    {
        uint id = CreateOutputSurface(m_size);
        if (id)
        {
            m_surfaces.push_back(id);
        }
        else
        {
            ok = false;
            break;
        }
    }

    if (m_surfaces.size() >= MIN_OUTPUT_SURFACES)
    {
        m_flipReady = m_flipQueue;
        VERBOSE(VB_GENERAL, LOC + QString("Created %1 output surfaces.")
                                  .arg(m_surfaces.size()));
    }
    return ok;
}

bool MythRenderVDPAU::RegisterCallback(bool enable)
{
    INIT_ST
    if (vdp_preemption_callback_register && m_device)
    {
        vdp_st = vdp_preemption_callback_register(
                    m_device, enable ? &vdpau_preemption_callback : NULL,
                    (void*)this);
        CHECK_ST
    }
    else
        return false;

    return ok;
}

bool MythRenderVDPAU::GetBestScaling(void)
{
    for (int i = 0; i < NUM_SCALING_LEVELS; i++)
        if (IsFeatureAvailable(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i))
            m_best_scaling = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i;

    if (m_best_scaling)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("HQ scaling level %1 of %2 available.")
            .arg(m_best_scaling - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + 1)
            .arg(NUM_SCALING_LEVELS));
    }

    return true;
}

bool MythRenderVDPAU::IsFeatureAvailable(uint feature)
{
    INIT_ST
    VdpBool supported = false;
    vdp_st = vdp_video_mixer_query_feature_support(m_device,
                feature, &supported);
    CHECK_ST
    return ok && supported;
}

void MythRenderVDPAU::Destroy(void)
{
    DestroyPresentationQueue();
    DestroyPresentationSurfaces();
    DestroyOutputSurfaces();
    DestroyVideoSurfaces();
    DestroyBitmapSurfaces();
    DestroyDecoders();
    DestroyVideoMixers();
    DestroyLayers();
    RegisterCallback(false);
    DestroyDevice();
    ResetProcs();

    m_best_scaling = 0;
    m_master  = kMasterUI;
    m_size    = QSize();
    m_errored = false;
    memset(&m_rect, 0, sizeof(VdpRect));
    m_window  = 0;

    if (m_display)
    {
        delete m_display;
        m_display = NULL;
    }
}

void MythRenderVDPAU::DestroyDevice(void)
{
    vdp_get_error_string = NULL;
    vdp_get_proc_address = NULL;
    if (vdp_device_destroy && m_device)
    {
        vdp_device_destroy(m_device);
        m_device = 0;
    }
}

void MythRenderVDPAU::ResetProcs(void)
{
    vdp_device_destroy = NULL;
    vdp_video_surface_create = NULL;
    vdp_video_surface_destroy = NULL;
    vdp_video_surface_put_bits_y_cb_cr = NULL;
    vdp_output_surface_put_bits_native = NULL;
    vdp_output_surface_create = NULL;
    vdp_output_surface_destroy = NULL;
    vdp_output_surface_render_bitmap_surface = NULL;
    vdp_video_mixer_create = NULL;
    vdp_video_mixer_set_feature_enables = NULL;
    vdp_video_mixer_destroy = NULL;
    vdp_video_mixer_render = NULL;
    vdp_video_mixer_set_attribute_values = NULL;
    vdp_generate_csc_matrix = NULL;
    vdp_video_mixer_query_feature_support = NULL;
    vdp_presentation_queue_target_destroy = NULL;
    vdp_presentation_queue_create = NULL;
    vdp_presentation_queue_destroy = NULL;
    vdp_presentation_queue_display = NULL;
    vdp_presentation_queue_block_until_surface_idle = NULL;
    vdp_presentation_queue_target_create_x11 = NULL;
    vdp_presentation_queue_get_time = NULL;
    vdp_presentation_queue_set_background_color = NULL;
    vdp_decoder_create = NULL;
    vdp_decoder_destroy = NULL;
    vdp_decoder_render = NULL;
    vdp_bitmap_surface_create = NULL;
    vdp_bitmap_surface_destroy = NULL;
    vdp_bitmap_surface_put_bits_native = NULL;
    vdp_preemption_callback_register = NULL;
}

void MythRenderVDPAU::DestroyPresentationQueue(void)
{
    MythXLocker locker(m_display);
    if (vdp_presentation_queue_destroy && m_flipQueue)
    {
        vdp_presentation_queue_destroy(m_flipQueue);
        m_flipQueue = 0;
    }

    if (vdp_presentation_queue_target_destroy && m_flipTarget)
    {
        vdp_presentation_queue_target_destroy(m_flipTarget);
        m_flipTarget = 0;
    }
    m_flipReady = false;
}

void MythRenderVDPAU::DestroyPresentationSurfaces(void)
{
    for (int i = 0; i < m_surfaces.size(); i++)
        DestroyOutputSurface(m_surfaces[i]);
    m_surfaces.clear();
    m_flipReady = false;
}

void MythRenderVDPAU::DestroyOutputSurfaces(void)
{
    if (!vdp_output_surface_destroy)
        return;

    if (m_outputSurfaces.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned output surfaces."));

    INIT_ST
    QHash<uint, VDPAUOutputSurface>::iterator it;
    for (it = m_outputSurfaces.begin(); it != m_outputSurfaces.end(); ++it)
    {
        vdp_st = vdp_output_surface_destroy(it.value().m_id);
        CHECK_ST
    }
    m_outputSurfaces.clear();
}

void MythRenderVDPAU::DestroyVideoSurfaces(void)
{
    if (!vdp_video_surface_destroy)
        return;

    if (m_videoSurfaces.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned video surfaces."));

    INIT_ST
    QHash<uint, VDPAUVideoSurface>::iterator it;;
    for(it = m_videoSurfaces.begin(); it != m_videoSurfaces.end(); ++it)
    {
        vdp_st = vdp_video_surface_destroy(it.value().m_id);
        CHECK_ST
    }
    m_videoSurfaces.clear();
    m_videoSurfaceHash.clear();
}

void MythRenderVDPAU::DestroyLayers(void)
{
    if (m_layers.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned layers."));
    m_layers.clear();
}

void MythRenderVDPAU::DestroyBitmapSurfaces(void)
{
    if (!vdp_bitmap_surface_destroy)
        return;

    if (m_bitmapSurfaces.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned bitmap surfaces."));

    INIT_ST
    QHash<uint, VDPAUBitmapSurface>::iterator it;
    for (it = m_bitmapSurfaces.begin(); it != m_bitmapSurfaces.end(); ++it)
    {
        vdp_st = vdp_bitmap_surface_destroy(it.value().m_id);
        CHECK_ST
    }
    m_bitmapSurfaces.clear();
}

void MythRenderVDPAU::DestroyDecoders(void)
{
    if (!vdp_decoder_destroy)
        return;

    if (m_decoders.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned decoders."));

    INIT_ST
    QHash<uint, VDPAUDecoder>::iterator it;
    for (it = m_decoders.begin(); it != m_decoders.end(); ++it)
    {
        vdp_st = vdp_decoder_destroy(it.value().m_id);
        CHECK_ST
    }
    m_decoders.clear();
}

void MythRenderVDPAU::DestroyVideoMixers(void)
{
    if (!vdp_video_mixer_destroy)
        return;

    if (m_videoMixers.size())
        VERBOSE(VB_IMPORTANT, LOC_WARN + QString("Orphaned video mixers."));

    INIT_ST
    QHash<uint, VDPAUVideoMixer>::iterator it;
    for (it = m_videoMixers.begin(); it != m_videoMixers.end(); ++it)
    {
        vdp_st = vdp_video_mixer_destroy(it.value().m_id);
        CHECK_ST
    }
    m_videoMixers.clear();
}

bool MythRenderVDPAU::SetMixerAttribute(uint id,
                                        VdpVideoMixerAttribute attribute[1],
                                        void const *value[1])
{
    INIT_ST

    if (!m_videoMixers.contains(id))
        return false;

    vdp_st = vdp_video_mixer_set_attribute_values(m_videoMixers[id].m_id, 1,
                                                  attribute, value);
    CHECK_ST
    return ok;
}

void MythRenderVDPAU::Preempted(void)
{
    if (!m_preempted || m_recreating)
        return;

    VERBOSE(VB_IMPORTANT, LOC + "Attempting to re-create VDPAU resources.");
    m_recreating = true;
    m_flipReady  = false;
    ResetProcs();
    bool ok = CreateDevice();
    if (ok) ok = GetProcs();
    if (ok) ok = CreatePresentationQueue();
    if (ok) ok = SetColorKey(m_colorKey);
    if (ok) ok = RegisterCallback();

    if (ok && m_outputSurfaces.size())
    {
        // also need to update output surfaces referenced in VdpLayer structs
        QHash<uint ,uint> old_surfaces;
        QHash<uint, VDPAUOutputSurface>::iterator it;
        for (it = m_outputSurfaces.begin(); it != m_outputSurfaces.end(); ++it)
        {
            old_surfaces.insert(it.value().m_id, it.key());
            uint check = CreateOutputSurface(it.value().m_size,
                                             it.value().m_fmt,it.key());
            if (check != it.key())
                ok = false;
        }
        QHash<uint, uint>::iterator old;
        for (old = old_surfaces.begin(); old != old_surfaces.end(); ++old)
            old.value() = m_outputSurfaces[old.value()].m_id;
        QHash<uint, VDPAULayer>::iterator layers;
        for (layers = m_layers.begin(); layers != m_layers.end(); ++layers)
        {
            uint old = layers.value().m_layer.source_surface;
            if (old_surfaces.contains(old))
                layers.value().m_layer.source_surface = old_surfaces[old];
        }
        if (ok)
            VERBOSE(VB_IMPORTANT, LOC + "Re-created output surfaces.");
    }

    if (ok && m_bitmapSurfaces.size())
    {
        QHash<uint, VDPAUBitmapSurface>::iterator it;
        for (it = m_bitmapSurfaces.begin(); it != m_bitmapSurfaces.end(); ++it)
        {
            uint check = CreateBitmapSurface(it.value().m_size, 
                                             it.value().m_fmt, it.key());
            if (check != it.key())
                ok = false;
        }
        if (ok)
            VERBOSE(VB_IMPORTANT, LOC + "Re-created bitmap surfaces.");

    }

    if (ok && m_decoders.size())
    {
        QHash<uint, VDPAUDecoder>::iterator it;
        for (it = m_decoders.begin(); it != m_decoders.end(); ++it)
        {
            uint check = CreateDecoder(it.value().m_size, it.value().m_profile,
                                       it.value().m_max_refs, it.key());
            if (check != it.key())
                ok = false;
        }
        if (ok)
            VERBOSE(VB_IMPORTANT, LOC + "Re-created decoders.");
    }

    if (ok && m_videoMixers.size())
    {
        QHash<uint, VDPAUVideoMixer>::iterator it;
        for (it = m_videoMixers.begin(); it != m_videoMixers.end(); ++it)
        {
            uint check = CreateVideoMixer(it.value().m_size,
                                          it.value().m_layers,
                                          it.value().m_features,
                                          it.value().m_type, it.key());
            if (check != it.key())
                ok = false;
        }
        if (ok)
            VERBOSE(VB_IMPORTANT, LOC + "Re-created video mixers.");
    }

    // reset of hardware surfaces needs to be done in the correct thread
    m_reset_video_surfaces = true;
    QHash<uint, VDPAUVideoSurface>::iterator it;
    for (it = m_videoSurfaces.begin(); it != m_videoSurfaces.end(); ++it)
        it.value().m_needs_reset = true;

    ResetVideoSurfaces();

    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, LOC + "Failed to re-create VDPAU resources.");
        m_errored = true;
        return;
    }

    m_preempted  = false;
    m_recreating = false;
    m_flipReady = true;
    m_recreated = true;
}
    
void MythRenderVDPAU::ResetVideoSurfaces(void)
{
    LOCK_ALL

    bool ok = true;
    pthread_t this_thread = pthread_self();
    QHash<uint, VDPAUVideoSurface>::iterator it;
    int surfaces_owned = 0;

    // save map of existing surfaces and create new surfaces
    QHash<uint ,uint> old_surfaces;
    for (it = m_videoSurfaces.begin(); it != m_videoSurfaces.end(); ++it)
    {
        old_surfaces.insert(it.value().m_id, it.key());
        if ((it.value().m_owner == this_thread) && it.value().m_needs_reset)
        {
            uint check = CreateVideoSurface(it.value().m_size,
                                            it.value().m_type, it.key());
            if (check != it.key())
                ok = false;
            surfaces_owned++;
            it.value().m_needs_reset = false;
        }
    }

    if (!surfaces_owned)
        return;

    VERBOSE(VB_IMPORTANT, LOC +
        QString("Attempting to reset %1 video surfaces owned by this thread %2")
            .arg(surfaces_owned).arg(this_thread));
                            
    // update old surfaces to map old vdpvideosurface to new vdpvideosurface
    QHash<uint, uint>::iterator old;
    for (old = old_surfaces.begin(); old != old_surfaces.end(); ++old)
        old.value() = m_videoSurfaces[old.value()].m_id;
        
    // update any render structure surface entries
    for (it = m_videoSurfaces.begin(); it != m_videoSurfaces.end(); ++it)
    {
        // MPEG2
        uint fwd  = it.value().m_render.info.mpeg.forward_reference;
        uint back = it.value().m_render.info.mpeg.backward_reference;
        if (fwd != VDP_INVALID_HANDLE && old_surfaces.contains(fwd))
            it.value().m_render.info.mpeg.forward_reference = old_surfaces[fwd];
        if (back != VDP_INVALID_HANDLE && old_surfaces.contains(back))
            it.value().m_render.info.mpeg.backward_reference = old_surfaces[back];

        // H264
        for (uint i = 0; i < 16; i++)
        {
            uint ref = it.value().m_render.info.h264.referenceFrames[i].surface;
            if (ref != VDP_INVALID_HANDLE && old_surfaces.contains(ref))
                it.value().m_render.info.h264.referenceFrames[i].surface =
                    old_surfaces[ref];
        }
        // VC1
        fwd  = it.value().m_render.info.vc1.forward_reference;
        back = it.value().m_render.info.vc1.backward_reference;
        if (fwd != VDP_INVALID_HANDLE && old_surfaces.contains(fwd))
            it.value().m_render.info.vc1.forward_reference = old_surfaces[fwd];
        if (back != VDP_INVALID_HANDLE && old_surfaces.contains(back))
            it.value().m_render.info.vc1.backward_reference = old_surfaces[back];
    }

    if (ok)
        VERBOSE(VB_IMPORTANT, LOC + QString("Re-created %1 video surfaces.")
                                            .arg(surfaces_owned));
    else
        VERBOSE(VB_IMPORTANT, LOC_ERR +
            QString("Error re-creating video surfaces."));

    int remaining = 0;
    for (it = m_videoSurfaces.begin(); it != m_videoSurfaces.end(); ++it)
        if (it.value().m_needs_reset)
            remaining++;

    VERBOSE(VB_IMPORTANT,
        LOC + QString("%1 of %2 video surfaces still need to be reset")
        .arg(remaining).arg(m_videoSurfaces.size()));
        
    m_reset_video_surfaces = remaining;
    m_errored = !ok;
}