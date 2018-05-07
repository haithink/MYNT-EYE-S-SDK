#include "uvc/uvc.h"  // NOLINT

#include <glog/logging.h>

#include <chrono>
#include <iostream>

#include <windows.h>
#include <usbioctl.h>
#include <sstream>

#include "uvc.h"

#include <Shlwapi.h>        // For QISearch, etc.
#include <mfapi.h>          // For MFStartup, etc.
#include <mfidl.h>          // For MF_DEVSOURCE_*, etc.
#include <mfreadwrite.h>    // MFCreateSourceReaderFromMediaSource
#include <mferror.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

#include <uuids.h>
#include <vidcap.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <Cfgmgr32.h>

#pragma comment(lib, "cfgmgr32.lib")

#include <SetupAPI.h>
#include <WinUsb.h>

#include <functional>
#include <thread>
#include <chrono>
#include <algorithm>
#include <regex>
#include <map>

#include <strsafe.h>

MYNTEYE_BEGIN_NAMESPACE

namespace uvc
{

const std::map<uint32_t, uint32_t> fourcc_map = { { 0x56595559, 0x32595559 } ,{ 0x59555956, 0x59555932 }};     /* 'VYUY' => '2YUY','YUYV' => 'YUY2'. */

#define LOG_ERROR(severity, str)                                           \
  do {                                                                     \
    LOG(severity) << str << " error " << errno << ", " << strerror(errno); \
  } while (0)

struct throw_error {
  throw_error() = default;

  explicit throw_error(const std::string &s) {
    ss << s;
  }

  ~throw_error() noexcept(false) {
    throw std::runtime_error(ss.str());
    // throw device_error(ss.str());
  }

  template<class T>
  throw_error &operator<<(const T &val) {
    ss << val;
    return *this;
  }

  std::ostringstream ss;
};

template<class T> class com_ptr
{
  T * p;
  void ref(T * new_p) {
    if(p == new_p) return;
    unref();
    p = new_p;
    if(p) p->AddRef();
  }

  void unref() {
    if(p) {
      p->Release();
      p = nullptr;
    }
  }
public:
  com_ptr() : p() {}
  com_ptr(T * p) : com_ptr() {
    ref(p);
  }
  com_ptr(const com_ptr & r) : com_ptr(r.p) {}
  ~com_ptr() {
    unref();
  }

  operator T * () const {
    return p;
  }
  T & operator * () const {
    return *p;
  }
  T * operator -> () const {
    return p;
  }

  T ** operator & () {
    unref();
    return &p;
  }
  com_ptr & operator = (const com_ptr & r) {
    ref(r.p);
    return *this;
  }
};

static std::string win_to_utf(const WCHAR * s)
{
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, NULL, NULL);
  if(len == 0) throw_error() << "WideCharToMultiByte(...) returned 0 and GetLastError() is " << GetLastError();
  std::string buffer(len-1, ' ');
  len = WideCharToMultiByte(CP_UTF8, 0, s, -1, &buffer[0], (int)buffer.size()+1, NULL, NULL);
  if(len == 0) throw_error() << "WideCharToMultiByte(...) returned 0 and GetLastError() is " << GetLastError();
  return buffer;
}

static void check(const char *call, HRESULT hr)
{
  if (FAILED(hr)) {
    throw_error() << call << "(...) returned 0x" << std::hex << (uint32_t)hr;    
  }
}

std::vector<std::string> tokenize(std::string string, char separator)
{
  std::vector<std::string> tokens;
  std::string::size_type i1 = 0;
  while(true) {
    auto i2 = string.find(separator, i1);
    if(i2 == std::string::npos) {
      tokens.push_back(string.substr(i1));
      return tokens;
    }
    tokens.push_back(string.substr(i1, i2-i1));
    i1 = i2+1;
  }
}

bool parse_usb_path(int & vid, int & pid, int & mi, std::string & unique_id, const std::string & path)
{
  auto name = path;
  std::transform(begin(name), end(name), begin(name), ::tolower);
  auto tokens = tokenize(name, '#');
  if(tokens.size() < 1 || tokens[0] != R"(\\?\usb)") return false; // Not a USB device
  if(tokens.size() < 3) {
    LOG_ERROR(WARNING,"malformed usb device path:  " << name);
    return false;
  }

  auto ids = tokenize(tokens[1], '&');
  if(ids[0].size() != 8 || ids[0].substr(0,4) != "vid_" || !(std::istringstream(ids[0].substr(4,4)) >> std::hex >> vid)) {
    LOG_ERROR(WARNING,"malformed vid string: " << tokens[1]);
    return false;
  }

  if(ids[1].size() != 8 || ids[1].substr(0,4) != "pid_" || !(std::istringstream(ids[1].substr(4,4)) >> std::hex >> pid)) {
    LOG_ERROR(WARNING,"malformed pid string: " << tokens[1]);
    return false;
  }

  if(ids[2].size() != 5 || ids[2].substr(0,3) != "mi_" || !(std::istringstream(ids[2].substr(3,2)) >> mi)) {
    LOG_ERROR(WARNING,"malformed mi string: " << tokens[1]);
    return false;
  }

  ids = tokenize(tokens[2], '&');
  if(ids.size() < 2) {
    LOG_ERROR(WARNING,"malformed id string: " << tokens[2]);
    return false;
  }
  unique_id = ids[1];
  return true;
}

bool parse_usb_path_from_device_id(int & vid, int & pid, int & mi, std::string & unique_id, const std::string & device_id)
{
  auto name = device_id;
  std::transform(begin(name), end(name), begin(name), ::tolower);
  auto tokens = tokenize(name, '\\');
  if (tokens.size() < 1 || tokens[0] != R"(usb)") return false; // Not a USB device

  auto ids = tokenize(tokens[1], '&');
  if (ids[0].size() != 8 || ids[0].substr(0, 4) != "vid_" || !(std::istringstream(ids[0].substr(4, 4)) >> std::hex >> vid)) {
    LOG_ERROR(WARNING,"malformed vid string: " << tokens[1]);
    return false;
  }

  if (ids[1].size() != 8 || ids[1].substr(0, 4) != "pid_" || !(std::istringstream(ids[1].substr(4, 4)) >> std::hex >> pid)) {
    LOG_ERROR(WARNING,"malformed pid string: " << tokens[1]);
    return false;
  }

  if (ids[2].size() != 5 || ids[2].substr(0, 3) != "mi_" || !(std::istringstream(ids[2].substr(3, 2)) >> mi)) {
    LOG_ERROR(WARNING,"malformed mi string: " << tokens[1]);
    return false;
  }

  ids = tokenize(tokens[2], '&');
  if (ids.size() < 2) {
    LOG_ERROR(WARNING,"malformed id string: " + tokens[2]);
    return false;
  }
  unique_id = ids[1];
  return true;
}

struct context {
  context() {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  }
  ~context() {
    MFShutdown();
    CoUninitialize();
  }
};

class reader_callback : public IMFSourceReaderCallback
{
  std::weak_ptr<device> owner; // The device holds a reference to us, so use weak_ptr to prevent a cycle
  ULONG ref_count;
  volatile bool streaming = false;
public:
  reader_callback(std::weak_ptr<device> owner) : owner(owner), ref_count() {}

  bool is_streaming() const {
    return streaming;
  }
  void on_start() {
    streaming = true;
  }

#pragma warning( push )
#pragma warning( disable: 4838 )
  // Implement IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void ** ppvObject) override {
    static const QITAB table[] = {QITABENT(reader_callback, IUnknown), QITABENT(reader_callback, IMFSourceReaderCallback), {0}};
    return QISearch(this, table, riid, ppvObject);
  }
#pragma warning( pop )

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = InterlockedDecrement(&ref_count);
    if(count == 0) delete this;
    return count;
  }

  // Implement IMFSourceReaderCallback
  HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample * sample) override;
  HRESULT STDMETHODCALLTYPE OnFlush(DWORD dwStreamIndex) override { streaming = false; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEvent(DWORD dwStreamIndex, IMFMediaEvent *pEvent) override { return S_OK; }
};

struct device {
  const std::shared_ptr<context> parent;
  int vid, pid;
  std::string unique_id;
  std::string name;

  com_ptr<reader_callback> reader_callback;
  com_ptr<IMFActivate> mf_activate;
  com_ptr<IMFMediaSource> mf_media_source;
  com_ptr<IAMCameraControl> am_camera_control;
  com_ptr<IAMVideoProcAmp> am_video_proc_amp;
  std::map<int, com_ptr<IKsControl>> ks_controls;
  com_ptr<IMFSourceReader> mf_source_reader;
  video_channel_callback callback = nullptr;

  device(std::shared_ptr<context> parent, int vid, int pid, std::string unique_id, std::string name) : parent(move(parent)), vid(vid), pid(pid), unique_id(move(unique_id)), name(name)
  {}

  ~device() {
    stop_streaming();
  }


  IKsControl * get_ks_control(const uvc::xu & xu) {
    auto it = ks_controls.find(xu.node);
    if(it != end(ks_controls)) return it->second;

    get_media_source();

    // Attempt to retrieve IKsControl
    com_ptr<IKsTopologyInfo> ks_topology_info = NULL;
    check("QueryInterface", mf_media_source->QueryInterface(__uuidof(IKsTopologyInfo), (void **)&ks_topology_info));

    GUID node_type;
    check("get_NodeType", ks_topology_info->get_NodeType(xu.node, &node_type));
    //const GUID KSNODETYPE_DEV_SPECIFIC_LOCAL{0x6BDD1FC6, 0X810F, 0x11D0, {0xBE, 0xC7 ,0x08, 0x00, 0x2B, 0xE2, 0x09, 0x2F}};
    //if(node_type != KSNODETYPE_DEV_SPECIFIC_LOCAL) throw_error() << "Invalid extension unit node ID: " << xu.node;

    com_ptr<IUnknown> unknown;
    check("CreateNodeInstance", ks_topology_info->CreateNodeInstance(xu.node, IID_IUnknown, (LPVOID *)&unknown));

    com_ptr<IKsControl> ks_control;
    check("QueryInterface", unknown->QueryInterface(__uuidof(IKsControl), (void **)&ks_control));
    VLOG(2) << "Obtained KS control node" << xu.node;
    return ks_controls[xu.node] = ks_control;
  }

  void start_streaming() {
    if(mf_source_reader) {
      reader_callback->on_start();
      check("IMFSourceReader::ReadSample", mf_source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL));
    }
  }

  void stop_streaming() {

    if(mf_source_reader) mf_source_reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    while(true) {
      bool is_streaming = reader_callback->is_streaming();
      if(is_streaming) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      else break;
    }

    mf_source_reader = nullptr;
    am_camera_control = nullptr;
    am_video_proc_amp = nullptr;
    ks_controls.clear();
    if(mf_media_source) {
      mf_media_source = nullptr;
      check("IMFActivate::ShutdownObject", mf_activate->ShutdownObject());
    }
    callback = {};
  }

  com_ptr<IMFMediaSource> get_media_source() {
    if(!mf_media_source) {
      check("IMFActivate::ActivateObject", mf_activate->ActivateObject(__uuidof(IMFMediaSource), (void **)&mf_media_source));
      if (mf_media_source) {
        check("IMFMediaSource::QueryInterface", mf_media_source->QueryInterface(__uuidof(IAMCameraControl), (void **)&am_camera_control));
        if (SUCCEEDED(mf_media_source->QueryInterface(__uuidof(IAMVideoProcAmp), (void **)&am_video_proc_amp)));
      } else throw_error() << "Invalid media source";
    }
    return mf_media_source;
  }

};

HRESULT reader_callback::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample * sample)
{
  if(auto owner_ptr = owner.lock()) {
    if(sample) {
      com_ptr<IMFMediaBuffer> buffer = NULL;
      if(SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
        BYTE * byte_buffer;
        DWORD max_length, current_length;
        if(SUCCEEDED(buffer->Lock(&byte_buffer, &max_length, &current_length))) {
          auto continuation = [buffer, this]() {
            buffer->Unlock();
          };
          owner_ptr->callback(byte_buffer);
        }
      }
    }

    if (auto owner_ptr_new = owner.lock()) {
      auto hr = owner_ptr_new->mf_source_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL);
      switch (hr) {
        case S_OK:
          break;
        case MF_E_INVALIDREQUEST:
          LOG_ERROR(WARNING,"ReadSample returned MF_E_INVALIDREQUEST");
          break;
        case MF_E_INVALIDSTREAMNUMBER:
          LOG_ERROR(WARNING,"ReadSample returned MF_E_INVALIDSTREAMNUMBER");
          break;
        case MF_E_NOTACCEPTING:
          LOG_ERROR(WARNING,"ReadSample returned MF_E_NOTACCEPTING");
          break;
        case E_INVALIDARG:
          LOG_ERROR(WARNING,"ReadSample returned E_INVALIDARG");
          break;
        case MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED:
          LOG_ERROR(WARNING,"ReadSample returned MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED");
          break;
        default:
          LOG_ERROR(WARNING,"ReadSample returned HRESULT " << std::hex << (uint32_t)hr);
          break;
      }
      if (hr != S_OK) streaming = false;
    }
  }
  return S_OK;
}

std::shared_ptr<context> create_context()
{
  return std::make_shared<context>();
}

std::vector<std::shared_ptr<device>> query_devices(std::shared_ptr<context> context)
{
  IMFAttributes *pAttributes = NULL;
  check("MFCreateAttributes", MFCreateAttributes(&pAttributes, 1));
  check("IMFAttributes::SetGUID",pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

  IMFActivate **ppDevices;
  UINT32 numDevices;
  check("MFEnumDeviceSources",MFEnumDeviceSources(pAttributes, &ppDevices, &numDevices));

  std::vector<std::shared_ptr<device>> devices;
  for (UINT32 i = 0; i < numDevices; ++i) {
    com_ptr<IMFActivate> pDevice;
    *&pDevice = ppDevices[i];

    WCHAR *wchar_dev_name = NULL;
    WCHAR *wchar_name = NULL;
    UINT32 length;

    pDevice->GetAllocatedString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &wchar_dev_name,
      &length);
    auto dev_name = win_to_utf(wchar_dev_name);
    CoTaskMemFree(wchar_dev_name);

    pDevice->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wchar_name, &length);
    auto name = win_to_utf(wchar_name); // Device description name
    CoTaskMemFree(wchar_name);

    int vid, pid, mi;
    std::string unique_id;

    if (!parse_usb_path(vid, pid, mi, unique_id, dev_name)) continue;

    std::shared_ptr<device> dev;
    for(auto & d : devices) {
      if(d->vid == vid && d->pid == pid && d->unique_id == unique_id)
        dev = d;
    }
    if(!dev) {
      try {
        dev = std::make_shared<device>(context, vid, pid, unique_id, name);
        devices.push_back(dev);
      } catch (const std::exception &e) {
        VLOG(2) << "Not a USB video device: " << e.what();
      }

    }

    dev->reader_callback = new reader_callback(dev);
    dev->mf_activate = pDevice;
    dev->vid = vid;
    dev->pid = pid;

  }

  CoTaskMemFree(ppDevices);
  return devices;
}

int get_vendor_id(const device &device)
{
  return device.vid;
}

int get_product_id(const device &device)
{
  return device.pid;
}

std::string get_name(const device &device)
{
  return device.name;
}

std::string get_video_name(const device &device)
{
  return device.name;
}

static long get_cid(Option option)
{
  switch (option) {
    case Option::GAIN:
      return VideoProcAmp_Gain;
    case Option::BRIGHTNESS:
      return VideoProcAmp_Brightness;
    case Option::CONTRAST:
      return VideoProcAmp_Contrast;
    default:
      LOG(FATAL) << "No VideoProcAmp cid for " << option;
  }
}

bool pu_control_range(
  const device &device, Option option, int32_t *min, int32_t *max,
  int32_t *def)
{
  const_cast<uvc::device &>(device).get_media_source();
  long minVal=0, maxVal=0, steppingDelta=0, defVal=0, capsFlag=0;
  check("IAMVideoProcAmp::GetRange", const_cast<uvc::device &>(device).am_video_proc_amp->GetRange(get_cid(option), &minVal, &maxVal, &steppingDelta, &defVal, &capsFlag));
  if(min)  *min  = static_cast<int>(minVal);
  if(max)  *max  = static_cast<int>(maxVal);
  if(def)  *def  = static_cast<int>(defVal);
  return true;
}

void get_pu_control(const device &device, long property, int32_t *value)
{
  long data, flags=0;
  check("IAMVideoProcAmp::Get", const_cast<uvc::device &>(device).am_video_proc_amp->Get(property, &data, &flags));
  *value = data;
}

void set_pu_control(const device &device, long property, int32_t *value)
{
  long data = *value;
  check("IAMVideoProcAmp::Set", const_cast<uvc::device &>(device).am_video_proc_amp->Set(property, data, VideoProcAmp_Flags_Auto));
}

bool pu_control_query(
  const device &device, Option option, pu_query query, int32_t *value)
{
  CHECK_NOTNULL(value);
  const_cast<uvc::device &>(device).get_media_source();
  switch (query) {
    case PU_QUERY_SET:
      set_pu_control(device, get_cid(option), value);
      return true;
    case PU_QUERY_GET:
      get_pu_control(device, get_cid(option), value);
      return true;
    default:
      LOG(ERROR) << "pu_control_query request code is unaccepted";
      return false;
  }
}
/*
void get_extension_control_range(const device &device, const xu &xu, uint8_t selector, xu_query query, uint8_t *data)
{
  CHECK_NOTNULL(data);
  int offset = 0;
  auto ks_control = const_cast<uvc::device &>(device).get_ks_control(xu);

  // get step, min and max values
  KSP_NODE node;
  memset(&node, 0, sizeof(KSP_NODE));
  node.Property.Set = reinterpret_cast<const GUID &>(xu.id);
  node.Property.Id = selector;
  node.NodeId = xu.node;

  switch (query) {
    case XU_QUERY_MIN:
      offset = 1;
      node.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_MAX:
      offset = 2;
      node.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_DEF:
      offset = 0;
      node.Property.Flags = KSPROPERTY_TYPE_DEFAULTVALUES | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    default:
      LOG(ERROR) << "xu request code is unaccepted";
      break;
  }

  KSPROPERTY_DESCRIPTION description;
  unsigned long bytes_received = 0;
  check("IKsControl::KsProperty", ks_control->KsProperty(
          (PKSPROPERTY)&node,
          sizeof(node),
          &description,
          sizeof(KSPROPERTY_DESCRIPTION),
          &bytes_received));

  unsigned long size = description.DescriptionSize;
  std::vector<BYTE> buffer((long)size);

  check("IKsControl::KsProperty", ks_control->KsProperty(
          (PKSPROPERTY)&node,
          sizeof(node),
          buffer.data(),
          size,
          &bytes_received));

  if (bytes_received != size) {
    throw  std::runtime_error("wrong data");
  }

  BYTE * pRangeValues = buffer.data() + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_DESCRIPTION);
  * data = (uint8_t)*(pRangeValues + offset);

}
*/
bool xu_control_query(
  const device &device, const xu &xu, uint8_t selector, xu_query query,
  uint16_t size, uint8_t *data)
{
  CHECK_NOTNULL(data);
  int offset = 0;
  int range_offset = sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_DESCRIPTION);

  auto ks_control = const_cast<uvc::device &>(device).get_ks_control(xu);
  KSP_NODE node;
  memset(&node, 0, sizeof(KSP_NODE));
  node.Property.Set = reinterpret_cast<const GUID &>(xu.id);
  node.Property.Id = selector;
  node.NodeId = xu.node;
  unsigned long bytes_received = 0;
  switch (query) {
    case XU_QUERY_SET:
      node.Property.Flags = KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_GET:
      node.Property.Flags = KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_MIN:
      offset = 1;
      node.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_MAX:
      offset = 2;
      node.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    case XU_QUERY_DEF:
      node.Property.Flags = KSPROPERTY_TYPE_DEFAULTVALUES | KSPROPERTY_TYPE_TOPOLOGY;
      break;
    default:
      return false;
  }
  check("IKsControl::KsProperty", ks_control->KsProperty((PKSPROPERTY)&node, sizeof(node), reinterpret_cast<void *>(data), size, &bytes_received));
  if (bytes_received != size) {
    throw_error() << "wrong data";
  }

  *data = (int)*(data+offset);

  return true;
}

void set_device_mode(device & device, int width, int height, int fourcc, int fps, video_channel_callback callback)
{
  if(!device.mf_source_reader) {
    com_ptr<IMFAttributes> pAttributes;
    check("MFCreateAttributes", MFCreateAttributes(&pAttributes, 1));
    check("IMFAttributes::SetUnknown", pAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IUnknown *>(device.reader_callback)));
    check("MFCreateSourceReaderFromMediaSource", MFCreateSourceReaderFromMediaSource(device.get_media_source(), pAttributes, &device.mf_source_reader));
  }

  if (fourcc_map.count(fourcc))   fourcc = fourcc_map.at(fourcc);

  for (DWORD j = 0; ; j++) {
    com_ptr<IMFMediaType> media_type;
    HRESULT hr = device.mf_source_reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, j, &media_type);
    if (hr == MF_E_NO_MORE_TYPES) break;
    check("IMFSourceReader::GetNativeMediaType", hr);

    UINT32 uvc_width, uvc_height, uvc_fps_num, uvc_fps_denom;
    GUID subtype;
    check("MFGetAttributeSize", MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &uvc_width, &uvc_height));
    if(uvc_width != width || uvc_height != height) continue;

    check("IMFMediaType::GetGUID", media_type->GetGUID(MF_MT_SUBTYPE, &subtype));
    if(subtype.Data1 != fourcc) continue;

    check("MFGetAttributeRatio", MFGetAttributeRatio(media_type, MF_MT_FRAME_RATE, &uvc_fps_num, &uvc_fps_denom));
    if(uvc_fps_denom == 0) continue;

    check("IMFSourceReader::SetCurrentMediaType", device.mf_source_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, media_type));

    device.callback = callback;
    return;
  }
  throw_error() << "no matching media type for pixel format " << std::hex << fourcc;
}

void start_streaming(device & device, int num_transfer_bufs)
{
  device.start_streaming();
}
void stop_streaming(device & device)
{
  device.stop_streaming();
}

}  // namespace uvc

MYNTEYE_END_NAMESPACE
