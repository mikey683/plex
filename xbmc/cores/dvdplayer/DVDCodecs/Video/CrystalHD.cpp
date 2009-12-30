/*
 *      Copyright (C) 2005-2009 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#endif

#if defined(HAVE_LIBCRYSTALHD)
#include "CrystalHD.h"
#include "DVDClock.h"
#include "utils/Atomics.h" 
#include "utils/Thread.h"
#include "utils/log.h"
#include "utils/fastmemcpy.h"

namespace BCM
{
#if defined(WIN32)
  typedef void		*HANDLE;
  #include "lib/crystalhd/include/bc_dts_defs.h"
  #include "lib/crystalhd/include/windows/bc_drv_if.h"
#else
  #ifndef __LINUX_USER__
  #define __LINUX_USER__
  #endif

  #include "libcrystalhd/bc_dts_types.h"
  #include "libcrystalhd/bc_dts_defs.h"
  #include "libcrystalhd/libcrystalhd_if.h"
#endif //defined(WIN32)
};


#if defined(WIN32)
#pragma comment(lib, "bcmDIL.lib")
#endif

#define __MODULE_NAME__ "CrystalHD"

void PrintFormat(BCM::BC_PIC_INFO_BLOCK &pib);

const char* g_DtsStatusText[] = {
	"BC_STS_SUCCESS",
	"BC_STS_INV_ARG",
	"BC_STS_BUSY",		
	"BC_STS_NOT_IMPL",		
	"BC_STS_PGM_QUIT",		
	"BC_STS_NO_ACCESS",	
	"BC_STS_INSUFF_RES",	
	"BC_STS_IO_ERROR",		
	"BC_STS_NO_DATA",		
	"BC_STS_VER_MISMATCH",
	"BC_STS_TIMEOUT",		
	"BC_STS_FW_CMD_ERR",	
	"BC_STS_DEC_NOT_OPEN",
	"BC_STS_ERR_USAGE",
	"BC_STS_IO_USER_ABORT",
	"BC_STS_IO_XFR_ERROR",
	"BC_STS_DEC_NOT_STARTED",
	"BC_STS_FWHEX_NOT_FOUND",
	"BC_STS_FMT_CHANGE",
	"BC_STS_HIF_ACCESS",
	"BC_STS_CMD_CANCELLED",
	"BC_STS_FW_AUTH_FAILED",
	"BC_STS_BOOTLOADER_FAILED",
	"BC_STS_CERT_VERIFY_ERROR",
	"BC_STS_DEC_EXIST_OPEN",
	"BC_STS_PENDING",
	"BC_STS_CLK_NOCHG"
};

////////////////////////////////////////////////////////////////////////////////////////////
class CMPCDecodeBuffer
{
public:
  CMPCDecodeBuffer(size_t size);
  CMPCDecodeBuffer(unsigned char* pBuffer, size_t size);
  virtual ~CMPCDecodeBuffer();
  size_t GetSize();
  unsigned char* GetPtr();
  void SetPts(uint64_t pts);
  uint64_t GetPts();
protected:
  size_t m_Size;
  unsigned char* m_pBuffer;
  unsigned int m_Id;
  uint64_t m_Pts;
};

////////////////////////////////////////////////////////////////////////////////////////////
class CMPCInputThread : public CThread
{
public:
  CMPCInputThread(BCM::HANDLE device);
  virtual ~CMPCInputThread();
  
  bool                AddInput(unsigned char* pData, size_t size, uint64_t pts);
  void                Flush(void);
  unsigned int        GetInputCount(void);
  
protected:
  CMPCDecodeBuffer*   AllocBuffer(size_t size);
  void                FreeBuffer(CMPCDecodeBuffer* pBuffer);
  CMPCDecodeBuffer*   GetNext(void);
  void                Process(void);

  CSyncPtrQueue<CMPCDecodeBuffer> m_InputList;
  
  BCM::HANDLE         m_Device;
  int                 m_SleepTime;
};

////////////////////////////////////////////////////////////////////////////////////////////
class CMPCOutputThread : public CThread
{
public:
  CMPCOutputThread(BCM::HANDLE device);
  virtual ~CMPCOutputThread();
  
  unsigned int        GetReadyCount(void);
  CPictureBuffer*     GetNextPicture(void);
  void                FreeBuffer(CPictureBuffer* pBuffer);
  void                Flush(void);

protected:
  void                SetFrameRate(uint32_t resolution);
  void                SetAspectRatio(BCM::BC_PIC_INFO_BLOCK *pic_info);
  bool                GetDecoderOutput(void);
  virtual void        Process(void);
  
  CSyncPtrQueue<CPictureBuffer> m_FreeList;
  CSyncPtrQueue<CPictureBuffer> m_ReadyList;

  BCM::HANDLE         m_Device;
  unsigned int        m_OutputTimeout;
  int                 m_width;
  int                 m_height;
  uint64_t            m_timestamp;
  int                 m_interlace;
  double              m_framerate;
  int                 m_aspectratio_x;
  int                 m_aspectratio_y;
};


////////////////////////////////////////////////////////////////////////////////////////////
#if defined(__APPLE__)
#pragma mark -
#endif
CMPCDecodeBuffer::CMPCDecodeBuffer(size_t size) :
m_Size(size)
{
  m_pBuffer = (unsigned char*)_aligned_malloc(size, 16);
}

CMPCDecodeBuffer::~CMPCDecodeBuffer()
{
  _aligned_free(m_pBuffer);
}

size_t CMPCDecodeBuffer::GetSize(void)
{
  return m_Size;
}

unsigned char* CMPCDecodeBuffer::GetPtr(void)
{
  return m_pBuffer;
}

void CMPCDecodeBuffer::SetPts(uint64_t pts)
{
  m_Pts = pts;
}

uint64_t CMPCDecodeBuffer::GetPts(void)
{
  return m_Pts;
}

///////////////////////////////////////////////////////////////////////////////////////////
#if defined(__APPLE__)
#pragma mark -
#endif
CMPCInputThread::CMPCInputThread(BCM::HANDLE device) :
  CThread(),
  m_Device(device),
  m_SleepTime(10)
{
}
  
CMPCInputThread::~CMPCInputThread()
{
  while (m_InputList.Count())
    delete m_InputList.Pop();
}

bool CMPCInputThread::AddInput(unsigned char* pData, size_t size, uint64_t pts)
{
  if (m_InputList.Count() > 75)
    return false;

  CMPCDecodeBuffer* pBuffer = AllocBuffer(size);
  fast_memcpy(pBuffer->GetPtr(), pData, size);
  pBuffer->SetPts(pts);
  m_InputList.Push(pBuffer);
  return true;
}

void CMPCInputThread::Flush(void)
{
  while (m_InputList.Count())
    delete m_InputList.Pop();
}

unsigned int CMPCInputThread::GetInputCount(void)
{
  return m_InputList.Count();
}

CMPCDecodeBuffer* CMPCInputThread::AllocBuffer(size_t size)
{
  return new CMPCDecodeBuffer(size);
}

void CMPCInputThread::FreeBuffer(CMPCDecodeBuffer* pBuffer)
{
  delete pBuffer;
}

CMPCDecodeBuffer* CMPCInputThread::GetNext(void)
{
  return m_InputList.Pop();
}

void CMPCInputThread::Process(void)
{
  CLog::Log(LOGDEBUG, "%s: Input Thread Started...", __MODULE_NAME__);
  CMPCDecodeBuffer* pInput = NULL;
  while (!m_bStop)
  {
    if (!pInput)
      pInput = GetNext();

    if (pInput)
    {
      BCM::BC_STATUS ret;
      BCM::BC_DTS_STATUS decoder_status;

      ret = BCM::DtsGetDriverStatus(m_Device, &decoder_status);
      if (ret == BCM::BC_STS_SUCCESS)
      {
        if (decoder_status.ReadyListCount <= 8)
        {
          BCM::BC_STATUS ret = BCM::DtsProcInput(m_Device, pInput->GetPtr(), pInput->GetSize(), pInput->GetPts(), FALSE);
          if (ret == BCM::BC_STS_SUCCESS)
          {
            delete pInput;
            pInput = NULL;
          }
          else if (ret == BCM::BC_STS_BUSY)
          {
            CLog::Log(LOGDEBUG, "%s: DtsProcInput returned BC_STS_BUSY", __MODULE_NAME__);
            Sleep(m_SleepTime); // Buffer is full
          }
        }
        else
        {
          //CLog::Log(LOGDEBUG, "%s: ReadyListCount > 5", __MODULE_NAME__);
          Sleep(m_SleepTime);
        }
      }
    }
    else
    {
      Sleep(m_SleepTime);
    }
  }

  CLog::Log(LOGDEBUG, "%s: Input Thread Stopped...", __MODULE_NAME__);
}

////////////////////////////////////////////////////////////////////////////////////////////
#if defined(__APPLE__)
#pragma mark -
#endif
CPictureBuffer::CPictureBuffer(int m_width, int m_height)
{
  m_field = CRYSTALHD_FIELD_FULL;
  m_interlace = false;
  m_timestamp = DVD_NOPTS_VALUE;
  m_PictureNumber = 0;
  m_y_buffer_size = m_width * m_height;
  m_uv_buffer_size = m_y_buffer_size / 2;
  m_y_buffer_ptr = (unsigned char*)_aligned_malloc(m_y_buffer_size, 16);
  m_uv_buffer_ptr = (unsigned char*)_aligned_malloc(m_uv_buffer_size, 16);
}

CPictureBuffer::~CPictureBuffer()
{
  if (m_y_buffer_ptr) _aligned_free(m_y_buffer_ptr);
  if (m_uv_buffer_ptr) _aligned_free(m_uv_buffer_ptr);
}

/////////////////////////////////////////////////////////////////////////////////////////////
#if defined(__APPLE__)
#pragma mark -
#endif
CMPCOutputThread::CMPCOutputThread(BCM::HANDLE device) :
  CThread(),
  m_Device(device),
  m_OutputTimeout(20)
{
}

CMPCOutputThread::~CMPCOutputThread()
{
  while(m_ReadyList.Count())
    delete m_ReadyList.Pop();
  while(m_FreeList.Count())
    delete m_FreeList.Pop();
}

unsigned int CMPCOutputThread::GetReadyCount(void)
{
  return m_ReadyList.Count();
}

CPictureBuffer* CMPCOutputThread::GetNextPicture(void)
{
  CPictureBuffer *pBuffer = m_ReadyList.Pop();
  return pBuffer;
}

void CMPCOutputThread::FreeBuffer(CPictureBuffer* pBuffer)
{
  m_FreeList.Push(pBuffer);
}

void CMPCOutputThread::Flush(void)
{
  while(m_ReadyList.Count())
  {
    m_FreeList.Push( m_ReadyList.Pop() );
  }
}

void CMPCOutputThread::SetFrameRate(uint32_t resolution)
{
  m_interlace = FALSE;
  
  switch (resolution) 
  {
    case BCM::vdecRESOLUTION_480p0:
      m_framerate = 60.0;
    break;
    case BCM::vdecRESOLUTION_576p0:
      m_framerate = 25.0;
    break;
    case BCM::vdecRESOLUTION_720p0:
      m_framerate = 60.0;
    break;
    case BCM::vdecRESOLUTION_1080p0:
      m_framerate = 24.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_480i0:
      m_framerate = 60.0 * 1000.0 / 1001.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_1080i0:
      m_framerate = 60.0 * 1000.0 / 1001.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_1080p23_976:
      m_framerate = 24.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_1080p29_97 :
      m_framerate = 30.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_1080p30  :
      m_framerate = 30.0;
    break;
    case BCM::vdecRESOLUTION_1080p24  :
      m_framerate = 24.0;
    break;
    case BCM::vdecRESOLUTION_1080p25 :
      m_framerate = 25.0;
    break;
    case BCM::vdecRESOLUTION_1080i29_97:
      m_framerate = 60.0 * 1000.0 / 1001.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_1080i25:
      m_framerate = 50.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_1080i:
      m_framerate = 60.0 * 1000.0 / 1001.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_720p59_94:
      m_framerate = 60.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_720p50:
      m_framerate = 50.0;
    break;
    case BCM::vdecRESOLUTION_720p:
      m_framerate = 60.0;
    break;
    case BCM::vdecRESOLUTION_720p23_976:
      m_framerate = 24.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_720p24:
      m_framerate = 25.0;
      break;
    case BCM::vdecRESOLUTION_720p29_97:
      m_framerate = 30.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_480i:
      m_framerate = 60.0 * 1000.0 / 1001.0;    
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_NTSC:
      m_framerate = 60.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_480p:
      m_framerate = 60.0;
    break;
    case BCM::vdecRESOLUTION_PAL1:
      m_framerate = 50.0;
      m_interlace = TRUE;
    break;
    case BCM::vdecRESOLUTION_480p23_976:
      m_framerate = 24.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_480p29_97:
      m_framerate = 30.0 * 1000.0 / 1001.0;
    break;
    case BCM::vdecRESOLUTION_576p25:
      m_framerate = 25.0;
    break;          
    default:
      m_framerate = 24.0 * 1000.0 / 1001.0;
    break;
  }
  
  if(m_interlace)
  {
    m_framerate /= 2;
  }
  CLog::Log(LOGDEBUG, "%s: resolution = %x  interlace = %d", __MODULE_NAME__, resolution, m_interlace);

}

void CMPCOutputThread::SetAspectRatio(BCM::BC_PIC_INFO_BLOCK *pic_info)
{
	switch(pic_info->aspect_ratio)
  {
    case BCM::vdecAspectRatioSquare:
      m_aspectratio_x = 1;
      m_aspectratio_y = 1;
    break;
    case BCM::vdecAspectRatio12_11:
      m_aspectratio_x = 12;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio10_11:
      m_aspectratio_x = 10;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio16_11:
      m_aspectratio_x = 16;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio40_33:
      m_aspectratio_x = 40;
      m_aspectratio_y = 33;
    break;
    case BCM::vdecAspectRatio24_11:
      m_aspectratio_x = 24;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio20_11:
      m_aspectratio_x = 20;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio32_11:
      m_aspectratio_x = 32;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio80_33:
      m_aspectratio_x = 80;
      m_aspectratio_y = 33;
    break;
    case BCM::vdecAspectRatio18_11:
      m_aspectratio_x = 18;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio15_11:
      m_aspectratio_x = 15;
      m_aspectratio_y = 11;
    break;
    case BCM::vdecAspectRatio64_33:
      m_aspectratio_x = 64;
      m_aspectratio_y = 33;
    break;
    case BCM::vdecAspectRatio160_99:
      m_aspectratio_x = 160;
      m_aspectratio_y = 99;
    break;
    case BCM::vdecAspectRatio4_3:
      m_aspectratio_x = 4;
      m_aspectratio_y = 3;
    break;
    case BCM::vdecAspectRatio16_9:
      m_aspectratio_x = 16;
      m_aspectratio_y = 9;
    break;
    case BCM::vdecAspectRatio221_1:
      m_aspectratio_x = 221;
      m_aspectratio_y = 1;
    break;
    case BCM::vdecAspectRatioUnknown:
      m_aspectratio_x = 0;
      m_aspectratio_y = 0;
    break;
    
    case BCM::vdecAspectRatioOther:
      m_aspectratio_x = pic_info->custom_aspect_ratio_width_height & 0x0000ffff;
      m_aspectratio_y = pic_info->custom_aspect_ratio_width_height >> 16;
    break;
  }
  if(m_aspectratio_x == 0)
  {
    m_aspectratio_x = 1;
    m_aspectratio_y = 1;
  }

  CLog::Log(LOGDEBUG, "%s: dec_par x = %d, dec_par y = %d", __MODULE_NAME__, m_aspectratio_x, m_aspectratio_y);
}

bool CMPCOutputThread::GetDecoderOutput(void)
{
  BCM::BC_STATUS ret;
  BCM::BC_DTS_PROC_OUT procOut;
  CPictureBuffer *pBuffer = NULL;
  bool got_picture = false;
  
  // Setup output struct
  memset(&procOut, 0, sizeof(BCM::BC_DTS_PROC_OUT));

  // Fetch data from the decoder
  ret = BCM::DtsProcOutputNoCopy(m_Device, m_OutputTimeout, &procOut);

  switch (ret)
  {
    case BCM::BC_STS_SUCCESS:
      if (procOut.PoutFlags & BCM::BC_POUT_FLAGS_PIB_VALID)
      {
        if (procOut.PicInfo.timeStamp && (procOut.PicInfo.timeStamp != m_timestamp))
        {
          m_timestamp = procOut.PicInfo.timeStamp;
          
          // Get next output buffer from the free list
          pBuffer = m_FreeList.Pop();
          if (!pBuffer)
          {
            // No free pre-allocated buffers so make one
            pBuffer = new CPictureBuffer(m_width, m_height); 
            CLog::Log(LOGDEBUG, "%s: Added a new Buffer, ReadyListCount: %d", __MODULE_NAME__, m_ReadyList.Count());    
          }
          
          pBuffer->m_width = m_width;
          pBuffer->m_height = m_height;
          pBuffer->m_field = CRYSTALHD_FIELD_FULL;
          pBuffer->m_interlace = m_interlace;
          pBuffer->m_framerate = m_framerate;
          pBuffer->m_timestamp = m_timestamp;
          pBuffer->m_PictureNumber = procOut.PicInfo.picture_number;

          int w = procOut.PicInfo.width;
          int h = procOut.PicInfo.height;

          switch(procOut.PicInfo.width)
          {
            case 720:
            case 1280:
            case 1920:
            {
              if (pBuffer->m_interlace)
              {
                // we get a 1/2 height frame (field) from hw, not seeing the odd/even flags so
                // can't tell which frames are odd, which are even so use picture number.
                unsigned char *s, *d;
                int stride = w*2;

                if (pBuffer->m_PictureNumber & 1)
                  pBuffer->m_field = CRYSTALHD_FIELD_ODD;
                else
                  pBuffer->m_field = CRYSTALHD_FIELD_EVEN;
                
                // copy y
                s = procOut.Ybuff;
                d = pBuffer->m_y_buffer_ptr;
                if (pBuffer->m_field == CRYSTALHD_FIELD_ODD)
                  d += stride;
                for (int y = 0; y < h/2; y++, s += w, d += stride)
                {
                  fast_memcpy(d, s, w);
                }
                // copy uv
                s = procOut.UVbuff;
                d = pBuffer->m_uv_buffer_ptr;
                if (pBuffer->m_field == CRYSTALHD_FIELD_ODD)
                  d += stride;
                for (int y = 0; y < h/4; y++, s += w, d += stride)
                {
                  fast_memcpy(d, s, w);
                }
              }
              else
              {
                // copy y
                fast_memcpy(pBuffer->m_y_buffer_ptr, procOut.Ybuff, pBuffer->m_y_buffer_size);
                // copy uv
                fast_memcpy(pBuffer->m_uv_buffer_ptr, procOut.UVbuff, pBuffer->m_uv_buffer_size);
              }
            }
            break;
            
            // frame that are not equal in width to 720, 1280 or 1920
            // need to be copied by a quantized stride (possible lib/driver bug).
            default:
            {
              unsigned char *s, *d;
              
              if (w < 720)
              {
                // copy y
                s = procOut.Ybuff;
                d = pBuffer->m_y_buffer_ptr;
                for (int y = 0; y < h; y++, s += 720, d += w)
                  fast_memcpy(d, s, w);
                // copy uv
                s = procOut.UVbuff;
                d = pBuffer->m_uv_buffer_ptr;
                for (int y = 0; y < h/2; y++, s += 720, d += w)
                  fast_memcpy(d, s, w);
              }
              else if (w < 1280)
              {
                // copy y
                s = procOut.Ybuff;
                d = pBuffer->m_y_buffer_ptr;
                for (int y = 0; y < h; y++, s += 1280, d += w)
                  fast_memcpy(d, s, w);
                // copy uv
                s = procOut.UVbuff;
                d = pBuffer->m_uv_buffer_ptr;
                for (int y = 0; y < h/2; y++, s += 1280, d += w)
                  fast_memcpy(d, s, w);
              }
              else
              {
                // copy y
                s = procOut.Ybuff;
                d = pBuffer->m_y_buffer_ptr;
                for (int y = 0; y < h; y++, s += 1920, d += w)
                  fast_memcpy(d, s, w);
                // copy uv
                s = procOut.UVbuff;
                d = pBuffer->m_uv_buffer_ptr;
                for (int y = 0; y < h/2; y++, s += 1920, d += w)
                  fast_memcpy(d, s, w);
              }
            }
            break;
          }

          m_ReadyList.Push(pBuffer);
          got_picture = true;
        }
        else
        {
          //CLog::Log(LOGDEBUG, "%s: Duplicate or no timestamp detected: %llu", __MODULE_NAME__, procOut.PicInfo.timeStamp); 
        }
      }

      BCM::DtsReleaseOutputBuffs(m_Device, NULL, FALSE);
    break;
      
    case BCM::BC_STS_NO_DATA:
    break;

    case BCM::BC_STS_FMT_CHANGE:
      CLog::Log(LOGDEBUG, "%s: Format Change Detected. Flags: 0x%08x", __MODULE_NAME__, procOut.PoutFlags); 
      if ((procOut.PoutFlags & BCM::BC_POUT_FLAGS_PIB_VALID) && (procOut.PoutFlags & BCM::BC_POUT_FLAGS_FMT_CHANGE))
      {
        PrintFormat(procOut.PicInfo);
        
        if (procOut.PicInfo.height == 1088) {
          procOut.PicInfo.height = 1080;
        }
        m_width = procOut.PicInfo.width;
        m_height = procOut.PicInfo.height;
        SetAspectRatio(&procOut.PicInfo);
        SetFrameRate(procOut.PicInfo.frame_rate);
        m_OutputTimeout = 2000;
      }
    break;
    
    default:
      if (ret > 26)
        CLog::Log(LOGDEBUG, "%s: DtsProcOutput returned %d.", __MODULE_NAME__, ret);
      else
        CLog::Log(LOGDEBUG, "%s: DtsProcOutput returned %s.", __MODULE_NAME__, g_DtsStatusText[ret]);
    break;
  }
  
  return got_picture;
}

void CMPCOutputThread::Process(void)
{
  bool got_picture;
  BCM::BC_STATUS ret;
  BCM::BC_DTS_STATUS decoder_status;

  CLog::Log(LOGDEBUG, "%s: Output Thread Started...", __MODULE_NAME__);
  while (!m_bStop)
  {
    got_picture = false;
    
    ret = BCM::DtsGetDriverStatus(m_Device, &decoder_status);
    if (ret == BCM::BC_STS_SUCCESS)
    {
      if (decoder_status.ReadyListCount > 0)
      {
        got_picture = GetDecoderOutput();
        /*
        CLog::Log(LOGDEBUG, "%s: ReadyListCount %d FreeListCount %d PIBMissCount %d\n", __MODULE_NAME__,
          decoder_status.ReadyListCount, decoder_status.FreeListCount, decoder_status.PIBMissCount);
        CLog::Log(LOGDEBUG, "%s: FramesDropped %d FramesCaptured %d FramesRepeated %d\n", __MODULE_NAME__,
          decoder_status.FramesDropped, decoder_status.FramesCaptured, decoder_status.FramesRepeated);
        */
      }
    }
    
    if (!got_picture)
      Sleep(10);
  }
  CLog::Log(LOGDEBUG, "%s: Output Thread Stopped...", __MODULE_NAME__);
}

////////////////////////////////////////////////////////////////////////////////////////////
#if defined(__APPLE__)
#pragma mark -
#endif
union pts_union
{
  double  pts_d;
  int64_t pts_i;
};

static int64_t pts_dtoi(double pts)
{
  pts_union u;
  u.pts_d = pts;
  return u.pts_i;
}

static double pts_itod(int64_t pts)
{
  pts_union u;
  u.pts_i = pts;
  return u.pts_d;
}

CCrystalHD* CCrystalHD::m_pInstance = NULL;

CCrystalHD::CCrystalHD() :
  m_Device(NULL),
  m_IsConfigured(false),
  m_drop_state(false),
  m_pInputThread(NULL),
  m_pOutputThread(NULL)
{
  BCM::BC_STATUS res;
  //BCM::U32 mode = BCM::DTS_PLAYBACK_MODE | BCM::DTS_LOAD_FILE_PLAY_FW | BCM::DTS_PLAYBACK_DROP_RPT_MODE | BCM::DTS_SKIP_TX_CHK_CPB | DTS_DFLT_RESOLUTION(BCM::vdecRESOLUTION_720p23_976);
  uint32_t mode = BCM::DTS_PLAYBACK_MODE | BCM::DTS_LOAD_FILE_PLAY_FW | BCM::DTS_PLAYBACK_DROP_RPT_MODE | 
DTS_DFLT_RESOLUTION(BCM::vdecRESOLUTION_720p23_976);
  
  res = BCM::DtsDeviceOpen(&m_Device, mode);
  if (res != BCM::BC_STS_SUCCESS)
  {
    m_Device = NULL;
    CLog::Log(LOGERROR, "%s: Failed to open Broadcom Crystal HD Device", __MODULE_NAME__);
  }
  else
  {
    CLog::Log(LOGINFO, "%s: Opened Broadcom Crystal HD Device", __MODULE_NAME__);
  }
}

CCrystalHD::~CCrystalHD()
{
  if (m_IsConfigured)
    Close();
    
  if (m_Device)
  {
    BCM::DtsDeviceClose(m_Device);
    m_Device = NULL;
  }
  CLog::Log(LOGINFO, "%s: Closed Broadcom Crystal HD Device", __MODULE_NAME__);
}

void CCrystalHD::RemoveInstance(void)
{
  if (m_pInstance)
  {
    delete m_pInstance;
    m_pInstance = NULL;
  }
}

CCrystalHD* CCrystalHD::GetInstance(void)
{
  if (!m_pInstance)
  {
    m_pInstance = new CCrystalHD();
  }
  return m_pInstance;
}

bool CCrystalHD::Open(BCM_STREAM_TYPE stream_type, BCM_CODEC_TYPE codec_type)
{
  BCM::BC_STATUS res;
  
  if (m_IsConfigured)
    Close();

  uint32_t videoAlg = 0;
  switch (codec_type)
  {
    case BC_CODEC_ID_VC1:
      videoAlg = BCM::BC_VID_ALGO_VC1;
      m_pFormatName = "bcm-vc1";
    break;
    case BC_CODEC_ID_H264:
      videoAlg = BCM::BC_VID_ALGO_H264;
      m_pFormatName = "bcm-h264";
    break;
    case BC_CODEC_ID_MPEG2:
      videoAlg = BCM::BC_VID_ALGO_MPEG2;
      m_pFormatName = "bcm-mpeg2";
    break;
    default:
      return false;
    break;
  }

  do 
  {
    res = BCM::DtsOpenDecoder(m_Device, stream_type);
    if (res != BCM::BC_STS_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: Failed to open decoder", __MODULE_NAME__);
      break;
    }
    res = BCM::DtsSetVideoParams(m_Device, videoAlg, FALSE, FALSE, TRUE, 0x80000000 | BCM::vdecFrameRate23_97);
    if (res != BCM::BC_STS_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: Failed to set video params", __MODULE_NAME__);
      break;
    }
    res = BCM::DtsStartDecoder(m_Device);
    if (res != BCM::BC_STS_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: Failed to start decoder", __MODULE_NAME__);
      break;
    }
    res = BCM::DtsStartCapture(m_Device);
    if (res != BCM::BC_STS_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: Failed to start capture", __MODULE_NAME__);
      break;
    }
 
    m_pInputThread = new CMPCInputThread(m_Device);
    m_pInputThread->Create();
    m_pOutputThread = new CMPCOutputThread(m_Device);
    m_pOutputThread->Create();

    m_drop_state = false;
    m_IsConfigured = true;
    CLog::Log(LOGDEBUG, "%s: Opened Broadcom Crystal HD Codec", __MODULE_NAME__);
  } while(false);
  
  return m_IsConfigured;
}

void CCrystalHD::Close(void)
{
  if (m_pInputThread)
  {
    m_pInputThread->StopThread();
    delete m_pInputThread;
    m_pInputThread = NULL;
  }
  if (m_pOutputThread)
  {
    while(m_BusyList.Count())
      m_pOutputThread->FreeBuffer( m_BusyList.Pop() );
      
    m_pOutputThread->StopThread();
    delete m_pOutputThread;
    m_pOutputThread = NULL;
  }

  if (m_Device)
  {
    BCM::DtsFlushRxCapture(m_Device, TRUE);
    BCM::DtsStopDecoder(m_Device);
    BCM::DtsCloseDecoder(m_Device);
  }
  m_IsConfigured = false;

  CLog::Log(LOGDEBUG, "%s: Closed Broadcom Crystal HD Codec", __MODULE_NAME__);
}

bool CCrystalHD::IsOpenforDecode(void)
{
  return m_IsConfigured;
}

void CCrystalHD::Flush(void)
{
  m_pInputThread->Flush();
  m_pOutputThread->Flush();

  CLog::Log(LOGDEBUG, "%s: Flush...", __MODULE_NAME__);
}

unsigned int CCrystalHD::GetInputCount(void)
{
  if (m_pInputThread)
    return m_pInputThread->GetInputCount();
  else
    return 0;  
}

bool CCrystalHD::AddInput(unsigned char *pData, size_t size, double pts)
{
  if (m_pInputThread)
    return m_pInputThread->AddInput(pData, size, pts_dtoi(pts) );
  else
    return false;
}

int CCrystalHD::GetReadyCount(void)
{
  if (m_pOutputThread)
    return m_pOutputThread->GetReadyCount();
  else
    return 0;
}

void CCrystalHD::ClearBusyList(void)
{
  if (m_pOutputThread)
  {
    // leave one around, DVDPlayer expects it
    while( m_BusyList.Count() > 1)
      m_pOutputThread->FreeBuffer( m_BusyList.Pop() );
  }
}

bool CCrystalHD::GetPicture(DVDVideoPicture *pDvdVideoPicture)
{
  CPictureBuffer* pBuffer = m_pOutputThread->GetNextPicture();

  pDvdVideoPicture->pts = pts_itod(pBuffer->m_timestamp);
  pDvdVideoPicture->iWidth = pBuffer->m_width;
  pDvdVideoPicture->iHeight = pBuffer->m_height;
  pDvdVideoPicture->iDisplayWidth = pBuffer->m_width;
  pDvdVideoPicture->iDisplayHeight = pBuffer->m_height;
  
  // Y plane
  pDvdVideoPicture->data[0] = (BYTE*)pBuffer->m_y_buffer_ptr;
  pDvdVideoPicture->iLineSize[0] = pBuffer->m_width;
  // UV packed plane
  pDvdVideoPicture->data[1] = (BYTE*)pBuffer->m_uv_buffer_ptr;
  pDvdVideoPicture->iLineSize[1] = pBuffer->m_width;
  // unused
  pDvdVideoPicture->data[2] = NULL;
  pDvdVideoPicture->iLineSize[2] = 0;

  pDvdVideoPicture->iRepeatPicture = 0;
  pDvdVideoPicture->iDuration = 0;
  //pDvdVideoPicture->iDuration = (DVD_TIME_BASE / pBuffer->m_framerate);
  pDvdVideoPicture->color_range = 1;
  // todo 
  //pDvdVideoPicture->color_matrix = 1;
  pDvdVideoPicture->iFlags = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->iFlags |= pBuffer->m_interlace ? DVP_FLAG_INTERLACED : 0;
  //if (m_interlace)
  //  pDvdVideoPicture->iFlags |= DVP_FLAG_TOP_FIELD_FIRST;
  pDvdVideoPicture->iFlags |= m_drop_state ? DVP_FLAG_DROPPED : 0;
  pDvdVideoPicture->format = DVDVideoPicture::FMT_NV12;

  m_BusyList.Push(pBuffer);
  
  //CLog::Log(LOGDEBUG, "%s: GetPicture, pts = %f", __MODULE_NAME__, pDvdVideoPicture->pts);
  return true;
}

void CCrystalHD::SetDropState(bool bDrop)
{
  if (m_drop_state != bDrop)
  {
    m_drop_state = bDrop;
    if (m_drop_state)
    {
      BCM::DtsSetFFRate(m_Device, 2);
    }
    else
    {
      BCM::DtsSetFFRate(m_Device, 1);
    }
    
    CLog::Log(LOGDEBUG, "%s: SetDropState... %d", __MODULE_NAME__, m_drop_state);
  }
}


////////////////////////////////////////////////////////////////////////////////////////////
void PrintFormat(BCM::BC_PIC_INFO_BLOCK &pib)
{
  CLog::Log(LOGDEBUG, "----------------------------------\n%s","");
  CLog::Log(LOGDEBUG, "\tTimeStamp: %llu\n", pib.timeStamp);
  CLog::Log(LOGDEBUG, "\tPicture Number: %d\n", pib.picture_number);
  CLog::Log(LOGDEBUG, "\tWidth: %d\n", pib.width);
  CLog::Log(LOGDEBUG, "\tHeight: %d\n", pib.height);
  CLog::Log(LOGDEBUG, "\tChroma: 0x%03x\n", pib.chroma_format);
  CLog::Log(LOGDEBUG, "\tPulldown: %d\n", pib.pulldown);         
  CLog::Log(LOGDEBUG, "\tFlags: 0x%08x\n", pib.flags);        
  CLog::Log(LOGDEBUG, "\tFrame Rate/Res: %d\n", pib.frame_rate);       
  CLog::Log(LOGDEBUG, "\tAspect Ratio: %d\n", pib.aspect_ratio);     
  CLog::Log(LOGDEBUG, "\tColor Primaries: %d\n", pib.colour_primaries);
  CLog::Log(LOGDEBUG, "\tMetaData: %d\n", pib.picture_meta_payload);
  CLog::Log(LOGDEBUG, "\tSession Number: %d\n", pib.sess_num);
  CLog::Log(LOGDEBUG, "\tTimeStamp: %d\n", pib.ycom);
  CLog::Log(LOGDEBUG, "\tCustom Aspect: %d\n", pib.custom_aspect_ratio_width_height);
  CLog::Log(LOGDEBUG, "\tFrames to Drop: %d\n", pib.n_drop);
  CLog::Log(LOGDEBUG, "\tH264 Valid Fields: 0x%08x\n", pib.other.h264.valid);
}


#endif
