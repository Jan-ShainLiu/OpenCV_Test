// fbc_cv is free software and uses the same licence as OpenCV
// Email: fengbingchun@163.com

#include "core/types.hpp"
#include "dshow.hpp"

// reference: 2.4.13.6
//            highgui/src/cap_dshow.cpp

#ifdef _MSC_VER

namespace fbc {

interface ISampleGrabberCB : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE SampleCB(
	double SampleTime,
	IMediaSample *pSample) = 0;

	virtual HRESULT STDMETHODCALLTYPE BufferCB(
		double SampleTime,
		BYTE *pBuffer,
		LONG BufferLen) = 0;

	virtual ~ISampleGrabberCB() {}
};

interface ISampleGrabber : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE SetOneShot(
	BOOL OneShot) = 0;

	virtual HRESULT STDMETHODCALLTYPE SetMediaType(
		const AM_MEDIA_TYPE *pType) = 0;

	virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(
		AM_MEDIA_TYPE *pType) = 0;

	virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(
		BOOL BufferThem) = 0;

	virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(
		LONG *pBufferSize,
		LONG *pBuffer) = 0;

	virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(
		IMediaSample **ppSample) = 0;

	virtual HRESULT STDMETHODCALLTYPE SetCallback(
		ISampleGrabberCB *pCallback,
		LONG WhichMethodToCallback) = 0;

	virtual ~ISampleGrabber() {}
};

///////////////////////////  HANDY FUNCTIONS  /////////////////////////////
static void MyFreeMediaType(AM_MEDIA_TYPE& mt){
	if (mt.cbFormat != 0)
	{
		CoTaskMemFree((PVOID)mt.pbFormat);
		mt.cbFormat = 0;
		mt.pbFormat = NULL;
	}
	if (mt.pUnk != NULL)
	{
		// Unecessary because pUnk should not be used, but safest.
		mt.pUnk->Release();
		mt.pUnk = NULL;
	}
}

static void MyDeleteMediaType(AM_MEDIA_TYPE *pmt)
{
	if (pmt != NULL)
	{
		MyFreeMediaType(*pmt);
		CoTaskMemFree(pmt);
	}
}

//////////////////////////////  CALLBACK  ////////////////////////////////
//Callback class
class SampleGrabberCallback : public ISampleGrabberCB{
public:
	//------------------------------------------------
	SampleGrabberCallback(){
		InitializeCriticalSection(&critSection);
		freezeCheck = 0;


		bufferSetup = false;
		newFrame = false;
		latestBufferLength = 0;

		hEvent = CreateEvent(NULL, true, false, NULL);
	}

	//------------------------------------------------
	virtual ~SampleGrabberCallback(){
		ptrBuffer = NULL;
		DeleteCriticalSection(&critSection);
		CloseHandle(hEvent);
		if (bufferSetup){
			delete[] pixels;
		}
	}

	//------------------------------------------------
	bool setupBuffer(int numBytesIn){
		if (bufferSetup){
			return false;
		}
		else{
			numBytes = numBytesIn;
			pixels = new unsigned char[numBytes];
			bufferSetup = true;
			newFrame = false;
			latestBufferLength = 0;
		}
		return true;
	}

	//------------------------------------------------
	STDMETHODIMP_(ULONG) AddRef() { return 1; }
	STDMETHODIMP_(ULONG) Release() { return 2; }

	//------------------------------------------------
	STDMETHODIMP QueryInterface(REFIID, void **ppvObject){
		*ppvObject = static_cast<ISampleGrabberCB*>(this);
		return S_OK;
	}

	//This method is meant to have less overhead
	//------------------------------------------------
	STDMETHODIMP SampleCB(double, IMediaSample *pSample){
		if (WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0) return S_OK;

		HRESULT hr = pSample->GetPointer(&ptrBuffer);

		if (hr == S_OK){
			latestBufferLength = pSample->GetActualDataLength();
			if (latestBufferLength == numBytes){
				EnterCriticalSection(&critSection);
				memcpy(pixels, ptrBuffer, latestBufferLength);
				newFrame = true;
				freezeCheck = 1;
				LeaveCriticalSection(&critSection);
				SetEvent(hEvent);
			}
			else{
				printf("ERROR: SampleCB() - buffer sizes do not match\n");
			}
		}

		return S_OK;
	}

	//This method is meant to have more overhead
	STDMETHODIMP BufferCB(double, BYTE *, long){
		return E_NOTIMPL;
	}

	int freezeCheck;

	int latestBufferLength;
	int numBytes;
	bool newFrame;
	bool bufferSetup;
	unsigned char * pixels;
	unsigned char * ptrBuffer;
	CRITICAL_SECTION critSection;
	HANDLE hEvent;
};

//////////////////////////////  VIDEO DEVICE  ////////////////////////////////
// ----------------------------------------------------------------------
//    Should this class also be the callback?
//
// ----------------------------------------------------------------------
videoDevice::videoDevice(){
	pCaptureGraph = NULL;    // Capture graph builder object
	pGraph = NULL;    // Graph builder object
	pControl = NULL;    // Media control object
	pVideoInputFilter = NULL; // Video Capture filter
	pGrabber = NULL; // Grabs frame
	pDestFilter = NULL; // Null Renderer Filter
	pGrabberF = NULL; // Grabber Filter
	pMediaEvent = NULL;
	streamConf = NULL;
	pAmMediaType = NULL;

	//This is our callback class that processes the frame.
	sgCallback = new SampleGrabberCallback();
	sgCallback->newFrame = false;

	//Default values for capture type
	videoType = MEDIASUBTYPE_RGB24;
	connection = PhysConn_Video_Composite;
	storeConn = 0;

	videoSize = 0;
	width = 0;
	height = 0;

	tryWidth = 640;
	tryHeight = 480;
	tryVideoType = MEDIASUBTYPE_RGB24;
	nFramesForReconnect = 10000;
	nFramesRunning = 0;
	myID = -1;

	tryDiffSize = true;
	useCrossbar = false;
	readyToCapture = false;
	sizeSet = false;
	setupStarted = false;
	specificFormat = false;
	autoReconnect = false;
	requestedFrameTime = -1;

	memset(wDeviceName, 0, sizeof(WCHAR)* 255);
	memset(nDeviceName, 0, sizeof(char)* 255);
}

// ----------------------------------------------------------------------
//    The only place we are doing new
//
// ----------------------------------------------------------------------
void videoDevice::setSize(int w, int h){
	if (sizeSet){
		if (verbose)printf("SETUP: Error device size should not be set more than once \n");
	}
	else
	{
		width = w;
		height = h;
		videoSize = w*h * 3;
		sizeSet = true;
		pixels = new unsigned char[videoSize];
		pBuffer = new char[videoSize];

		memset(pixels, 0, videoSize);
		sgCallback->setupBuffer(videoSize);

	}
}

// ----------------------------------------------------------------------
//    Borrowed from the SDK, use it to take apart the graph from
//  the capture device downstream to the null renderer
// ----------------------------------------------------------------------
void videoDevice::NukeDownstream(IBaseFilter *pBF){
	IPin *pP, *pTo;
	ULONG u;
	IEnumPins *pins = NULL;
	PIN_INFO pininfo;
	HRESULT hr = pBF->EnumPins(&pins);
	pins->Reset();
	while (hr == NOERROR)
	{
		hr = pins->Next(1, &pP, &u);
		if (hr == S_OK && pP)
		{
			pP->ConnectedTo(&pTo);
			if (pTo)
			{
				hr = pTo->QueryPinInfo(&pininfo);
				if (hr == NOERROR)
				{
					if (pininfo.dir == PINDIR_INPUT)
					{
						NukeDownstream(pininfo.pFilter);
						pGraph->Disconnect(pTo);
						pGraph->Disconnect(pP);
						pGraph->RemoveFilter(pininfo.pFilter);
					}
					pininfo.pFilter->Release();
					pininfo.pFilter = NULL;
				}
				pTo->Release();
			}
			pP->Release();
		}
	}
	if (pins) pins->Release();
}

// ----------------------------------------------------------------------
//    Also from SDK
// ----------------------------------------------------------------------
void videoDevice::destroyGraph(){
	HRESULT hr = 0;
	//int FuncRetval=0;
	//int NumFilters=0;

	int i = 0;
	while (hr == NOERROR)
	{
		IEnumFilters * pEnum = 0;
		ULONG cFetched;

		// We must get the enumerator again every time because removing a filter from the graph
		// invalidates the enumerator. We always get only the first filter from each enumerator.
		hr = pGraph->EnumFilters(&pEnum);
		if (FAILED(hr)) { if (verbose)printf("SETUP: pGraph->EnumFilters() failed. \n"); return; }

		IBaseFilter * pFilter = NULL;
		if (pEnum->Next(1, &pFilter, &cFetched) == S_OK)
		{
			FILTER_INFO FilterInfo;
			memset(&FilterInfo, 0, sizeof(FilterInfo));
			hr = pFilter->QueryFilterInfo(&FilterInfo);
			FilterInfo.pGraph->Release();

			int count = 0;
			char buffer[255];
			memset(buffer, 0, 255 * sizeof(char));

			while (FilterInfo.achName[count] != 0x00)
			{
				buffer[count] = (char)FilterInfo.achName[count];
				count++;
			}

			if (verbose)printf("SETUP: removing filter %s...\n", buffer);
			hr = pGraph->RemoveFilter(pFilter);
			if (FAILED(hr)) { if (verbose)printf("SETUP: pGraph->RemoveFilter() failed. \n"); return; }
			if (verbose)printf("SETUP: filter removed %s  \n", buffer);

			pFilter->Release();
			pFilter = NULL;
		}
		else break;
		pEnum->Release();
		pEnum = NULL;
		i++;
	}

	return;
}

// ----------------------------------------------------------------------
// Our deconstructor, attempts to tear down graph and release filters etc
// Does checking to make sure it only is freeing if it needs to
// Probably could be a lot cleaner! :)
// ----------------------------------------------------------------------
videoDevice::~videoDevice(){
	if (setupStarted){ if (verbose)printf("\nSETUP: Disconnecting device %i\n", myID); }
	else{
		if (sgCallback){
			sgCallback->Release();
			delete sgCallback;
		}
		return;
	}

	HRESULT HR = NOERROR;

	//Stop the callback and free it
	if ((sgCallback) && (pGrabber))
	{
		pGrabber->SetCallback(NULL, 1);
		if (verbose)printf("SETUP: freeing Grabber Callback\n");
		sgCallback->Release();

		//delete our pixels
		if (sizeSet){
			delete[] pixels;
			delete[] pBuffer;
		}

		delete sgCallback;
	}

	//Check to see if the graph is running, if so stop it.
	if ((pControl))
	{
		HR = pControl->Pause();
		if (FAILED(HR)) if (verbose)printf("ERROR - Could not pause pControl\n");

		HR = pControl->Stop();
		if (FAILED(HR)) if (verbose)printf("ERROR - Could not stop pControl\n");
	}

	//Disconnect filters from capture device
	if ((pVideoInputFilter))NukeDownstream(pVideoInputFilter);

	//Release and zero pointers to our filters etc
	if ((pDestFilter)){
		if (verbose)printf("SETUP: freeing Renderer \n");
		(pDestFilter)->Release();
		(pDestFilter) = 0;
	}
	if ((pVideoInputFilter)){
		if (verbose)printf("SETUP: freeing Capture Source \n");
		(pVideoInputFilter)->Release();
		(pVideoInputFilter) = 0;
	}
	if ((pGrabberF)){
		if (verbose)printf("SETUP: freeing Grabber Filter  \n");
		(pGrabberF)->Release();
		(pGrabberF) = 0;
	}
	if ((pGrabber)){
		if (verbose)printf("SETUP: freeing Grabber  \n");
		(pGrabber)->Release();
		(pGrabber) = 0;
	}
	if ((pControl)){
		if (verbose)printf("SETUP: freeing Control   \n");
		(pControl)->Release();
		(pControl) = 0;
	}
	if ((pMediaEvent)){
		if (verbose)printf("SETUP: freeing Media Event  \n");
		(pMediaEvent)->Release();
		(pMediaEvent) = 0;
	}
	if ((streamConf)){
		if (verbose)printf("SETUP: freeing Stream  \n");
		(streamConf)->Release();
		(streamConf) = 0;
	}

	if ((pAmMediaType)){
		if (verbose)printf("SETUP: freeing Media Type  \n");
		MyDeleteMediaType(pAmMediaType);
	}

	if ((pMediaEvent)){
		if (verbose)printf("SETUP: freeing Media Event  \n");
		(pMediaEvent)->Release();
		(pMediaEvent) = 0;
	}

	//Destroy the graph
	if ((pGraph))destroyGraph();

	//Release and zero our capture graph and our main graph
	if ((pCaptureGraph)){
		if (verbose)printf("SETUP: freeing Capture Graph \n");
		(pCaptureGraph)->Release();
		(pCaptureGraph) = 0;
	}
	if ((pGraph)){
		if (verbose)printf("SETUP: freeing Main Graph \n");
		(pGraph)->Release();
		(pGraph) = 0;
	}

	//delete our pointers
	delete pDestFilter;
	delete pVideoInputFilter;
	delete pGrabberF;
	delete pGrabber;
	delete pControl;
	delete streamConf;
	delete pMediaEvent;
	delete pCaptureGraph;
	delete pGraph;

	if (verbose)printf("SETUP: Device %i disconnected and freed\n\n", myID);
}

//////////////////////////////  VIDEO INPUT  ////////////////////////////////
////////////////////////////  PUBLIC METHODS  ///////////////////////////////
// ----------------------------------------------------------------------
// Constructor - creates instances of videoDevice and adds the various
// media subtypes to check.
// ----------------------------------------------------------------------
videoInput::videoInput(){
	//start com
	comInit();

	devicesFound = 0;
	callbackSetCount = 0;
	bCallback = true;

	//setup a max no of device objects
	for (int i = 0; i<VI_MAX_CAMERAS; i++)  VDList[i] = new videoDevice();

	if (verbose)printf("\n***** VIDEOINPUT LIBRARY - %2.04f - TFW07 *****\n\n", VI_VERSION);

	//added for the pixelink firewire camera
	//MEDIASUBTYPE_Y800 = (GUID)FOURCCMap(FCC('Y800'));
	//MEDIASUBTYPE_Y8   = (GUID)FOURCCMap(FCC('Y8'));
	//MEDIASUBTYPE_GREY = (GUID)FOURCCMap(FCC('GREY'));

	//The video types we support
	//in order of preference

	mediaSubtypes[0] = MEDIASUBTYPE_RGB24;
	mediaSubtypes[1] = MEDIASUBTYPE_RGB32;
	mediaSubtypes[2] = MEDIASUBTYPE_RGB555;
	mediaSubtypes[3] = MEDIASUBTYPE_RGB565;
	mediaSubtypes[4] = MEDIASUBTYPE_YUY2;
	mediaSubtypes[5] = MEDIASUBTYPE_YVYU;
	mediaSubtypes[6] = MEDIASUBTYPE_YUYV;
	mediaSubtypes[7] = MEDIASUBTYPE_IYUV;
	mediaSubtypes[8] = MEDIASUBTYPE_UYVY;
	mediaSubtypes[9] = MEDIASUBTYPE_YV12;
	mediaSubtypes[10] = MEDIASUBTYPE_YVU9;
	mediaSubtypes[11] = MEDIASUBTYPE_Y411;
	mediaSubtypes[12] = MEDIASUBTYPE_Y41P;
	mediaSubtypes[13] = MEDIASUBTYPE_Y211;
	mediaSubtypes[14] = MEDIASUBTYPE_AYUV;
	mediaSubtypes[15] = MEDIASUBTYPE_MJPG; // MGB

	//non standard
	mediaSubtypes[16] = MEDIASUBTYPE_Y800;
	mediaSubtypes[17] = MEDIASUBTYPE_Y8;
	mediaSubtypes[18] = MEDIASUBTYPE_GREY;
	mediaSubtypes[19] = MEDIASUBTYPE_I420;

	//The video formats we support
	formatTypes[VI_NTSC_M] = AnalogVideo_NTSC_M;
	formatTypes[VI_NTSC_M_J] = AnalogVideo_NTSC_M_J;
	formatTypes[VI_NTSC_433] = AnalogVideo_NTSC_433;

	formatTypes[VI_PAL_B] = AnalogVideo_PAL_B;
	formatTypes[VI_PAL_D] = AnalogVideo_PAL_D;
	formatTypes[VI_PAL_G] = AnalogVideo_PAL_G;
	formatTypes[VI_PAL_H] = AnalogVideo_PAL_H;
	formatTypes[VI_PAL_I] = AnalogVideo_PAL_I;
	formatTypes[VI_PAL_M] = AnalogVideo_PAL_M;
	formatTypes[VI_PAL_N] = AnalogVideo_PAL_N;
	formatTypes[VI_PAL_NC] = AnalogVideo_PAL_N_COMBO;

	formatTypes[VI_SECAM_B] = AnalogVideo_SECAM_B;
	formatTypes[VI_SECAM_D] = AnalogVideo_SECAM_D;
	formatTypes[VI_SECAM_G] = AnalogVideo_SECAM_G;
	formatTypes[VI_SECAM_H] = AnalogVideo_SECAM_H;
	formatTypes[VI_SECAM_K] = AnalogVideo_SECAM_K;
	formatTypes[VI_SECAM_K1] = AnalogVideo_SECAM_K1;
	formatTypes[VI_SECAM_L] = AnalogVideo_SECAM_L;
}

// ----------------------------------------------------------------------
// static - set whether messages get printed to console or not
//
// ----------------------------------------------------------------------
void videoInput::setVerbose(bool _verbose){
	verbose = _verbose;
}

// ----------------------------------------------------------------------
// change to use callback or regular capture
// callback tells you when a new frame has arrived
// but non-callback won't - but is single threaded
// ----------------------------------------------------------------------
void videoInput::setUseCallback(bool useCallback){
	if (callbackSetCount == 0){
		bCallback = useCallback;
		callbackSetCount = 1;
	}
	else{
		printf("ERROR: setUseCallback can only be called before setup\n");
	}
}

// ----------------------------------------------------------------------
// Set the requested framerate - no guarantee you will get this
//
// ----------------------------------------------------------------------
void videoInput::setIdealFramerate(int deviceNumber, int idealFramerate){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return;

	if (idealFramerate > 0){
		VDList[deviceNumber]->requestedFrameTime = (unsigned long)(10000000 / idealFramerate);
	}
}

// ----------------------------------------------------------------------
// Set the requested framerate - no guarantee you will get this
//
// ----------------------------------------------------------------------
void videoInput::setAutoReconnectOnFreeze(int deviceNumber, bool doReconnect, int numMissedFramesBeforeReconnect){
	if (deviceNumber >= VI_MAX_CAMERAS) return;

	VDList[deviceNumber]->autoReconnect = doReconnect;
	VDList[deviceNumber]->nFramesForReconnect = numMissedFramesBeforeReconnect;

}

// ----------------------------------------------------------------------
// Setup a device with the default settings
//
// ----------------------------------------------------------------------
bool videoInput::setupDevice(int deviceNumber){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return false;

	if (setup(deviceNumber))return true;
	return false;
}

// ----------------------------------------------------------------------
// Setup a device with the default size but specify input type
//
// ----------------------------------------------------------------------
bool videoInput::setupDevice(int deviceNumber, int _connection){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return false;

	setPhyCon(deviceNumber, _connection);
	if (setup(deviceNumber))return true;
	return false;
}

// ----------------------------------------------------------------------
// Setup a device with the default connection but specify size
//
// ----------------------------------------------------------------------
bool videoInput::setupDevice(int deviceNumber, int w, int h){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return false;

	setAttemptCaptureSize(deviceNumber, w, h);
	if (setup(deviceNumber))return true;
	return false;
}

// ----------------------------------------------------------------------
// Setup a device with the default connection but specify size and image format
//
// Note:
// Need a new name for this since signature clashes with ",int connection)"
// ----------------------------------------------------------------------
bool videoInput::setupDeviceFourcc(int deviceNumber, int w, int h, int fourcc){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return false;

	if (fourcc != -1) {
		GUID *mediaType = getMediaSubtypeFromFourcc(fourcc);
		if (mediaType) {
			setAttemptCaptureSize(deviceNumber, w, h, *mediaType);
		}
	}
	else {
		setAttemptCaptureSize(deviceNumber, w, h);
	}
	if (setup(deviceNumber))return true;
	return false;
}

// ----------------------------------------------------------------------
// Setup a device with specific size and connection
//
// ----------------------------------------------------------------------
bool videoInput::setupDevice(int deviceNumber, int w, int h, int _connection){
	if (deviceNumber >= VI_MAX_CAMERAS || VDList[deviceNumber]->readyToCapture) return false;

	setAttemptCaptureSize(deviceNumber, w, h);
	setPhyCon(deviceNumber, _connection);
	if (setup(deviceNumber))return true;
	return false;
}

// ----------------------------------------------------------------------
// Setup the default video format of the device
// Must be called after setup!
// See #define formats in header file (eg VI_NTSC_M )
//
// ----------------------------------------------------------------------
bool videoInput::setFormat(int deviceNumber, int format){
	if (deviceNumber >= VI_MAX_CAMERAS || !VDList[deviceNumber]->readyToCapture) return false;

	bool returnVal = false;

	if (format >= 0 && format < VI_NUM_FORMATS){
		VDList[deviceNumber]->formatType = formatTypes[format];
		VDList[deviceNumber]->specificFormat = true;

		if (VDList[deviceNumber]->specificFormat){

			HRESULT hr = getDevice(&VDList[deviceNumber]->pVideoInputFilter, deviceNumber, VDList[deviceNumber]->wDeviceName, VDList[deviceNumber]->nDeviceName);
			if (hr != S_OK){
				return false;
			}

			IAMAnalogVideoDecoder *pVideoDec = NULL;
			hr = VDList[deviceNumber]->pCaptureGraph->FindInterface(NULL, &MEDIATYPE_Video, VDList[deviceNumber]->pVideoInputFilter, IID_IAMAnalogVideoDecoder, (void **)&pVideoDec);


			//in case the settings window some how freed them first
			if (VDList[deviceNumber]->pVideoInputFilter)VDList[deviceNumber]->pVideoInputFilter->Release();
			if (VDList[deviceNumber]->pVideoInputFilter)VDList[deviceNumber]->pVideoInputFilter = NULL;

			if (FAILED(hr)){
				printf("SETUP: couldn't set requested format\n");
			}
			else{
				long lValue = 0;
				hr = pVideoDec->get_AvailableTVFormats(&lValue);
				if (SUCCEEDED(hr) && (lValue & VDList[deviceNumber]->formatType))
				{
					hr = pVideoDec->put_TVFormat(VDList[deviceNumber]->formatType);
					if (FAILED(hr)){
						printf("SETUP: couldn't set requested format\n");
					}
					else{
						returnVal = true;
					}
				}

				pVideoDec->Release();
				pVideoDec = NULL;
			}
		}
	}

	return returnVal;
}

// ----------------------------------------------------------------------
// Our static function for returning device names - thanks Peter!
// Must call listDevices first.
//
// ----------------------------------------------------------------------
char videoInput::deviceNames[VI_MAX_CAMERAS][255] = { { 0 } };

char * videoInput::getDeviceName(int deviceID){
	if (deviceID >= VI_MAX_CAMERAS){
		return NULL;
	}
	return deviceNames[deviceID];
}

// ----------------------------------------------------------------------
// Our static function for finding num devices available etc
//
// ----------------------------------------------------------------------
int videoInput::listDevices(bool silent){

	//COM Library Intialization
	comInit();

	if (!silent)printf("\nVIDEOINPUT SPY MODE!\n\n");

	ICreateDevEnum *pDevEnum = NULL;
	IEnumMoniker *pEnum = NULL;
	int deviceCounter = 0;

	HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL,
		CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
		reinterpret_cast<void**>(&pDevEnum));

	if (SUCCEEDED(hr))
	{
		// Create an enumerator for the video capture category.
		hr = pDevEnum->CreateClassEnumerator(
			CLSID_VideoInputDeviceCategory,
			&pEnum, 0);

		if (hr == S_OK){

			if (!silent)printf("SETUP: Looking For Capture Devices\n");
			IMoniker *pMoniker = NULL;

			while (pEnum->Next(1, &pMoniker, NULL) == S_OK){
				IPropertyBag *pPropBag;
				hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag,
					(void**)(&pPropBag));

				if (FAILED(hr)){
					pMoniker->Release();
					continue;  // Skip this one, maybe the next one will work.
				}

				// Find the description or friendly name.
				VARIANT varName;
				VariantInit(&varName);
				hr = pPropBag->Read(L"Description", &varName, 0);

				if (FAILED(hr)) hr = pPropBag->Read(L"FriendlyName", &varName, 0);

				if (SUCCEEDED(hr)){
					hr = pPropBag->Read(L"FriendlyName", &varName, 0);

					int count = 0;
					int maxLen = sizeof(deviceNames[0]) / sizeof(deviceNames[0][0]) - 2;
					while (varName.bstrVal[count] != 0x00 && count < maxLen) {
						deviceNames[deviceCounter][count] = (char)varName.bstrVal[count];
						count++;
					}
					deviceNames[deviceCounter][count] = 0;

					if (!silent)printf("SETUP: %i) %s \n", deviceCounter, deviceNames[deviceCounter]);
				}

				pPropBag->Release();
				pPropBag = NULL;

				pMoniker->Release();
				pMoniker = NULL;

				deviceCounter++;
			}

			pDevEnum->Release();
			pDevEnum = NULL;

			pEnum->Release();
			pEnum = NULL;
		}

		if (!silent)printf("SETUP: %i Device(s) found\n\n", deviceCounter);
	}

	comUnInit();

	return deviceCounter;
}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
int videoInput::getWidth(int id){

	if (isDeviceSetup(id))
	{
		return VDList[id]->width;
	}

	return 0;

}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
int videoInput::getHeight(int id){
	if (isDeviceSetup(id))
	{
		return VDList[id]->height;
	}

	return 0;

}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
int videoInput::getFourcc(int id){
	if (isDeviceSetup(id))
	{
		return getFourccFromMediaSubtype(VDList[id]->videoType);
	}

	return 0;

}

double videoInput::getFPS(int id){
	if (isDeviceSetup(id))
	{
		double frameTime = VDList[id]->requestedFrameTime;
		if (frameTime>0) {
			return (10000000.0 / frameTime);
		}
	}

	return 0;

}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
int videoInput::getSize(int id){
	if (isDeviceSetup(id))
	{
		return VDList[id]->videoSize;
	}

	return 0;

}

// ----------------------------------------------------------------------
// Uses a supplied buffer
// ----------------------------------------------------------------------
bool videoInput::getPixels(int id, unsigned char * dstBuffer, bool flipRedAndBlue, bool flipImage){
	bool success = false;

	if (isDeviceSetup(id)){
		if (bCallback){
			//callback capture

			DWORD result = WaitForSingleObject(VDList[id]->sgCallback->hEvent, 1000);
			if (result != WAIT_OBJECT_0) return false;

			//double paranoia - mutexing with both event and critical section
			EnterCriticalSection(&VDList[id]->sgCallback->critSection);

			unsigned char * src = VDList[id]->sgCallback->pixels;
			unsigned char * dst = dstBuffer;
			int height = VDList[id]->height;
			int width = VDList[id]->width;

			processPixels(src, dst, width, height, flipRedAndBlue, flipImage);
			VDList[id]->sgCallback->newFrame = false;

			LeaveCriticalSection(&VDList[id]->sgCallback->critSection);

			ResetEvent(VDList[id]->sgCallback->hEvent);

			success = true;
		}
		else{
			//regular capture method
			long bufferSize = VDList[id]->videoSize;
			HRESULT hr = VDList[id]->pGrabber->GetCurrentBuffer(&bufferSize, (long *)VDList[id]->pBuffer);
			if (hr == S_OK){
				int numBytes = VDList[id]->videoSize;
				if (numBytes == bufferSize){

					unsigned char * src = (unsigned char *)VDList[id]->pBuffer;
					unsigned char * dst = dstBuffer;
					int height = VDList[id]->height;
					int width = VDList[id]->width;

					processPixels(src, dst, width, height, flipRedAndBlue, flipImage);
					success = true;
				}
				else{
					if (verbose)printf("ERROR: GetPixels() - bufferSizes do not match!\n");
				}
			}
			else{
				if (verbose)printf("ERROR: GetPixels() - Unable to grab frame for device %i\n", id);
			}
		}
	}

	return success;
}

// ----------------------------------------------------------------------
// Returns a buffer
// ----------------------------------------------------------------------
unsigned char * videoInput::getPixels(int id, bool flipRedAndBlue, bool flipImage){

	if (isDeviceSetup(id)){
		getPixels(id, VDList[id]->pixels, flipRedAndBlue, flipImage);
	}

	return VDList[id]->pixels;
}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
bool videoInput::isFrameNew(int id){
	if (!isDeviceSetup(id)) return false;
	if (!bCallback)return true;

	bool result = false;
	bool freeze = false;

	//again super paranoia!
	EnterCriticalSection(&VDList[id]->sgCallback->critSection);
	result = VDList[id]->sgCallback->newFrame;

	//we need to give it some time at the begining to start up so lets check after 400 frames
	if (VDList[id]->nFramesRunning > 400 && VDList[id]->sgCallback->freezeCheck > VDList[id]->nFramesForReconnect){
		freeze = true;
	}

	//we increment the freezeCheck var here - the callback resets it to 1
	//so as long as the callback is running this var should never get too high.
	//if the callback is not running then this number will get high and trigger the freeze action below
	VDList[id]->sgCallback->freezeCheck++;
	LeaveCriticalSection(&VDList[id]->sgCallback->critSection);

	VDList[id]->nFramesRunning++;

	if (freeze && VDList[id]->autoReconnect){
		if (verbose)printf("ERROR: Device seems frozen - attempting to reconnect\n");
		if (!restartDevice(VDList[id]->myID)){
			if (verbose)printf("ERROR: Unable to reconnect to device\n");
		}
		else{
			if (verbose)printf("SUCCESS: Able to reconnect to device\n");
		}
	}

	return result;
}

// ----------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------
bool videoInput::isDeviceSetup(int id){

	if (id<devicesFound && VDList[id]->readyToCapture)return true;
	else return false;

}

// ----------------------------------------------------------------------
// Gives us a little pop up window to adjust settings
// We do this in a seperate thread now!
// ----------------------------------------------------------------------
void __cdecl videoInput::basicThread(void * objPtr){

	//get a reference to the video device
	//not a copy as we need to free the filter
	videoDevice * vd = *((videoDevice **)(objPtr));
	ShowFilterPropertyPages(vd->pVideoInputFilter);

	//now we free the filter and make sure it set to NULL
	if (vd->pVideoInputFilter)vd->pVideoInputFilter->Release();
	if (vd->pVideoInputFilter)vd->pVideoInputFilter = NULL;

	return;
}

void videoInput::showSettingsWindow(int id){
	if (isDeviceSetup(id)){
		//HANDLE myTempThread;

		//we reconnect to the device as we have freed our reference to it
		//why have we freed our reference? because there seemed to be an issue
		//with some mpeg devices if we didn't
		HRESULT hr = getDevice(&VDList[id]->pVideoInputFilter, id, VDList[id]->wDeviceName, VDList[id]->nDeviceName);
		if (hr == S_OK){
			//myTempThread = (HANDLE)
			_beginthread(basicThread, 0, (void *)&VDList[id]);
		}
	}
}

// Set a video signal setting using IAMVideoProcAmp
bool videoInput::getVideoSettingFilter(int deviceID, long Property, long &min, long &max, long &SteppingDelta, long &currentValue, long &flags, long &defaultValue){
	if (!isDeviceSetup(deviceID))return false;

	HRESULT hr;
	//bool isSuccessful = false;

	videoDevice * VD = VDList[deviceID];

	hr = getDevice(&VD->pVideoInputFilter, deviceID, VD->wDeviceName, VD->nDeviceName);
	if (FAILED(hr)){
		printf("setVideoSetting - getDevice Error\n");
		return false;
	}

	IAMVideoProcAmp *pAMVideoProcAmp = NULL;

	hr = VD->pVideoInputFilter->QueryInterface(IID_IAMVideoProcAmp, (void**)&pAMVideoProcAmp);
	if (FAILED(hr)){
		printf("setVideoSetting - QueryInterface Error\n");
		if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
		if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;
		return false;
	}

	char propStr[16];
	getVideoPropertyAsString(Property, propStr);

	if (verbose) printf("Setting video setting %s.\n", propStr);

	pAMVideoProcAmp->GetRange(Property, &min, &max, &SteppingDelta, &defaultValue, &flags);
	if (verbose) printf("Range for video setting %s: Min:%ld Max:%ld SteppingDelta:%ld Default:%ld Flags:%ld\n", propStr, min, max, SteppingDelta, defaultValue, flags);
	pAMVideoProcAmp->Get(Property, &currentValue, &flags);

	if (pAMVideoProcAmp)pAMVideoProcAmp->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;

	return true;

}

// Set a video signal setting using IAMVideoProcAmp
bool videoInput::setVideoSettingFilterPct(int deviceID, long Property, float pctValue, long Flags){
	if (!isDeviceSetup(deviceID))return false;

	long min, max, currentValue, flags, defaultValue, stepAmnt;

	if (!getVideoSettingFilter(deviceID, Property, min, max, stepAmnt, currentValue, flags, defaultValue))return false;

	if (pctValue > 1.0)pctValue = 1.0;
	else if (pctValue < 0)pctValue = 0.0;

	float range = (float)max - (float)min;
	if (range <= 0)return false;
	if (stepAmnt == 0) return false;

	long value = (long)((float)min + range * pctValue);
	long rasterValue = value;

	//if the range is the stepAmnt then it is just a switch
	//so we either set the value to low or high
	if (range == stepAmnt){
		if (pctValue < 0.5)rasterValue = min;
		else rasterValue = max;
	}
	else{
		//we need to rasterize the value to the stepping amnt
		long mod = value % stepAmnt;
		float halfStep = (float)stepAmnt * 0.5f;
		if (mod < halfStep) rasterValue -= mod;
		else rasterValue += stepAmnt - mod;
		printf("RASTER - pctValue is %f - value is %li - step is %li - mod is %li - rasterValue is %li\n", pctValue, value, stepAmnt, mod, rasterValue);
	}

	return setVideoSettingFilter(deviceID, Property, rasterValue, Flags, false);
}

// Set a video signal setting using IAMVideoProcAmp
bool videoInput::setVideoSettingFilter(int deviceID, long Property, long lValue, long Flags, bool useDefaultValue){
	if (!isDeviceSetup(deviceID))return false;

	HRESULT hr;
	//bool isSuccessful = false;

	char propStr[16];
	getVideoPropertyAsString(Property, propStr);

	videoDevice * VD = VDList[deviceID];

	hr = getDevice(&VD->pVideoInputFilter, deviceID, VD->wDeviceName, VD->nDeviceName);
	if (FAILED(hr)){
		printf("setVideoSetting - getDevice Error\n");
		return false;
	}

	IAMVideoProcAmp *pAMVideoProcAmp = NULL;

	hr = VD->pVideoInputFilter->QueryInterface(IID_IAMVideoProcAmp, (void**)&pAMVideoProcAmp);
	if (FAILED(hr)){
		printf("setVideoSetting - QueryInterface Error\n");
		if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
		if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;
		return false;
	}

	if (verbose) printf("Setting video setting %s.\n", propStr);
	long CurrVal, Min, Max, SteppingDelta, Default, CapsFlags, AvailableCapsFlags = 0;


	pAMVideoProcAmp->GetRange(Property, &Min, &Max, &SteppingDelta, &Default, &AvailableCapsFlags);
	if (verbose) printf("Range for video setting %s: Min:%ld Max:%ld SteppingDelta:%ld Default:%ld Flags:%ld\n", propStr, Min, Max, SteppingDelta, Default, AvailableCapsFlags);
	pAMVideoProcAmp->Get(Property, &CurrVal, &CapsFlags);

	if (verbose) printf("Current value: %ld Flags %ld (%s)\n", CurrVal, CapsFlags, (CapsFlags == 1 ? "Auto" : (CapsFlags == 2 ? "Manual" : "Unknown")));

	if (useDefaultValue) {
		pAMVideoProcAmp->Set(Property, Default, VideoProcAmp_Flags_Auto);
	}
	else{
		// Perhaps add a check that lValue and Flags are within the range aquired from GetRange above
		pAMVideoProcAmp->Set(Property, lValue, Flags);
	}

	if (pAMVideoProcAmp)pAMVideoProcAmp->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;

	return true;

}

bool videoInput::setVideoSettingCameraPct(int deviceID, long Property, float pctValue, long Flags){
	if (!isDeviceSetup(deviceID))return false;

	long min, max, currentValue, flags, defaultValue, stepAmnt;

	if (!getVideoSettingCamera(deviceID, Property, min, max, stepAmnt, currentValue, flags, defaultValue))return false;

	if (pctValue > 1.0)pctValue = 1.0;
	else if (pctValue < 0)pctValue = 0.0;

	float range = (float)max - (float)min;
	if (range <= 0)return false;
	if (stepAmnt == 0) return false;

	long value = (long)((float)min + range * pctValue);
	long rasterValue = value;

	//if the range is the stepAmnt then it is just a switch
	//so we either set the value to low or high
	if (range == stepAmnt){
		if (pctValue < 0.5)rasterValue = min;
		else rasterValue = max;
	}
	else{
		//we need to rasterize the value to the stepping amnt
		long mod = value % stepAmnt;
		float halfStep = (float)stepAmnt * 0.5f;
		if (mod < halfStep) rasterValue -= mod;
		else rasterValue += stepAmnt - mod;
		printf("RASTER - pctValue is %f - value is %li - step is %li - mod is %li - rasterValue is %li\n", pctValue, value, stepAmnt, mod, rasterValue);
	}

	return setVideoSettingCamera(deviceID, Property, rasterValue, Flags, false);
}

bool videoInput::setVideoSettingCamera(int deviceID, long Property, long lValue, long Flags, bool useDefaultValue){
	IAMCameraControl *pIAMCameraControl;
	if (isDeviceSetup(deviceID))
	{
		HRESULT hr;
		hr = getDevice(&VDList[deviceID]->pVideoInputFilter, deviceID, VDList[deviceID]->wDeviceName, VDList[deviceID]->nDeviceName);

		char propStr[16];
		getCameraPropertyAsString(Property, propStr);

		if (verbose) printf("Setting video setting %s.\n", propStr);
		hr = VDList[deviceID]->pVideoInputFilter->QueryInterface(IID_IAMCameraControl, (void**)&pIAMCameraControl);
		if (FAILED(hr)) {
			printf("Error\n");
			if (VDList[deviceID]->pVideoInputFilter)VDList[deviceID]->pVideoInputFilter->Release();
			if (VDList[deviceID]->pVideoInputFilter)VDList[deviceID]->pVideoInputFilter = NULL;
			return false;
		}
		else
		{
			long CurrVal, Min, Max, SteppingDelta, Default, CapsFlags, AvailableCapsFlags;
			pIAMCameraControl->GetRange(Property, &Min, &Max, &SteppingDelta, &Default, &AvailableCapsFlags);
			if (verbose) printf("Range for video setting %s: Min:%ld Max:%ld SteppingDelta:%ld Default:%ld Flags:%ld\n", propStr, Min, Max, SteppingDelta, Default, AvailableCapsFlags);
			pIAMCameraControl->Get(Property, &CurrVal, &CapsFlags);
			if (verbose) printf("Current value: %ld Flags %ld (%s)\n", CurrVal, CapsFlags, (CapsFlags == 1 ? "Auto" : (CapsFlags == 2 ? "Manual" : "Unknown")));
			if (useDefaultValue) {
				pIAMCameraControl->Set(Property, Default, CameraControl_Flags_Auto);
			}
			else
			{
				// Perhaps add a check that lValue and Flags are within the range aquired from GetRange above
				pIAMCameraControl->Set(Property, lValue, Flags);
			}
			pIAMCameraControl->Release();
			if (VDList[deviceID]->pVideoInputFilter)VDList[deviceID]->pVideoInputFilter->Release();
			if (VDList[deviceID]->pVideoInputFilter)VDList[deviceID]->pVideoInputFilter = NULL;
			return true;
		}
	}
	return false;
}

bool videoInput::getVideoSettingCamera(int deviceID, long Property, long &min, long &max, long &SteppingDelta, long &currentValue, long &flags, long &defaultValue){
	if (!isDeviceSetup(deviceID))return false;

	HRESULT hr;
	//bool isSuccessful = false;

	videoDevice * VD = VDList[deviceID];

	hr = getDevice(&VD->pVideoInputFilter, deviceID, VD->wDeviceName, VD->nDeviceName);
	if (FAILED(hr)){
		printf("setVideoSetting - getDevice Error\n");
		return false;
	}

	IAMCameraControl *pIAMCameraControl = NULL;

	hr = VD->pVideoInputFilter->QueryInterface(IID_IAMCameraControl, (void**)&pIAMCameraControl);
	if (FAILED(hr)){
		printf("setVideoSetting - QueryInterface Error\n");
		if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
		if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;
		return false;
	}

	char propStr[16];
	getCameraPropertyAsString(Property, propStr);
	if (verbose) printf("Setting video setting %s.\n", propStr);

	pIAMCameraControl->GetRange(Property, &min, &max, &SteppingDelta, &defaultValue, &flags);
	if (verbose) printf("Range for video setting %s: Min:%ld Max:%ld SteppingDelta:%ld Default:%ld Flags:%ld\n", propStr, min, max, SteppingDelta, defaultValue, flags);
	pIAMCameraControl->Get(Property, &currentValue, &flags);

	if (pIAMCameraControl)pIAMCameraControl->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter->Release();
	if (VD->pVideoInputFilter)VD->pVideoInputFilter = NULL;

	return true;

}

// ----------------------------------------------------------------------
// Shutsdown the device, deletes the object and creates a new object
// so it is ready to be setup again
// ----------------------------------------------------------------------
void videoInput::stopDevice(int id){
	if (id < VI_MAX_CAMERAS)
	{
		delete VDList[id];
		VDList[id] = new videoDevice();
	}

}

// ----------------------------------------------------------------------
// Restarts the device with the same settings it was using
//
// ----------------------------------------------------------------------
bool videoInput::restartDevice(int id){
	if (isDeviceSetup(id))
	{
		int conn = VDList[id]->storeConn;
		int tmpW = VDList[id]->width;
		int tmpH = VDList[id]->height;

		bool bFormat = VDList[id]->specificFormat;
		long format = VDList[id]->formatType;

		int nReconnect = VDList[id]->nFramesForReconnect;
		bool bReconnect = VDList[id]->autoReconnect;

		unsigned long avgFrameTime = VDList[id]->requestedFrameTime;

		stopDevice(id);

		//set our fps if needed
		if (avgFrameTime != (unsigned long)-1){
			VDList[id]->requestedFrameTime = avgFrameTime;
		}

		if (setupDevice(id, tmpW, tmpH, conn)){
			//reapply the format - ntsc / pal etc
			if (bFormat){
				setFormat(id, format);
			}
			if (bReconnect){
				setAutoReconnectOnFreeze(id, true, nReconnect);
			}
			return true;
		}
	}
	return false;
}

// ----------------------------------------------------------------------
// Shuts down all devices, deletes objects and unitializes com if needed
//
// ----------------------------------------------------------------------
videoInput::~videoInput(){

	for (int i = 0; i < VI_MAX_CAMERAS; i++)
	{
		delete VDList[i];
	}
	//Unitialize com
	comUnInit();
}

//////////////////////////////  VIDEO INPUT  ////////////////////////////////
////////////////////////////  PRIVATE METHODS  //////////////////////////////
// ----------------------------------------------------------------------
// We only should init com if it hasn't been done so by our apps thread
// Use a static counter to keep track of other times it has been inited
// (do we need to worry about multithreaded apps?)
// ----------------------------------------------------------------------
bool videoInput::comInit(){
	/*HRESULT hr = NOERROR;

	//no need for us to start com more than once
	if(comInitCount == 0 ){

	// Initialize the COM library.
	//CoInitializeEx so videoInput can run in another thread
	#ifdef VI_COM_MULTI_THREADED
	hr = CoInitializeEx(NULL,COINIT_MULTITHREADED);
	#else
	hr = CoInitialize(NULL);
	#endif
	//this is the only case where there might be a problem
	//if another library has started com as single threaded
	//and we need it multi-threaded - send warning but don't fail
	if( hr == RPC_E_CHANGED_MODE){
	if(verbose)printf("SETUP - COM already setup - threaded VI might not be possible\n");
	}
	}

	comInitCount++;*/
	return true;
}

// ----------------------------------------------------------------------
// Same as above but to unitialize com, decreases counter and frees com
// if no one else is using it
// ----------------------------------------------------------------------
bool videoInput::comUnInit(){
	/*if(comInitCount > 0)comInitCount--;        //decrease the count of instances using com

	if(comInitCount == 0){
	CoUninitialize();    //if there are no instances left - uninitialize com
	return true;
	}

	return false;*/
	return true;
}

// ----------------------------------------------------------------------
// This is the size we ask for - we might not get it though :)
//
// ----------------------------------------------------------------------
void videoInput::setAttemptCaptureSize(int id, int w, int h, GUID mediaType){

	VDList[id]->tryWidth = w;
	VDList[id]->tryHeight = h;
	VDList[id]->tryDiffSize = true;
	VDList[id]->tryVideoType = mediaType;

}

// ----------------------------------------------------------------------
// Set the connection type
// (maybe move to private?)
// ----------------------------------------------------------------------
void videoInput::setPhyCon(int id, int conn){
	switch (conn){

	case 0:
		VDList[id]->connection = PhysConn_Video_Composite;
		break;
	case 1:
		VDList[id]->connection = PhysConn_Video_SVideo;
		break;
	case 2:
		VDList[id]->connection = PhysConn_Video_Tuner;
		break;
	case 3:
		VDList[id]->connection = PhysConn_Video_USB;
		break;
	case 4:
		VDList[id]->connection = PhysConn_Video_1394;
		break;
	default:
		return; //if it is not these types don't set crossbar
		break;
	}

	VDList[id]->storeConn = conn;
	VDList[id]->useCrossbar = true;
}

// ----------------------------------------------------------------------
// Check that we are not trying to setup a non-existant device
// Then start the graph building!
// ----------------------------------------------------------------------
bool videoInput::setup(int deviceNumber){
	devicesFound = getDeviceCount();

	if (deviceNumber>devicesFound - 1)
	{
		if (verbose)printf("SETUP: device[%i] not found - you have %i devices available\n", deviceNumber, devicesFound);
		if (devicesFound >= 0) if (verbose)printf("SETUP: this means that the last device you can use is device[%i] \n", devicesFound - 1);
		return false;
	}

	if (VDList[deviceNumber]->readyToCapture)
	{
		if (verbose)printf("SETUP: can't setup, device %i is currently being used\n", VDList[deviceNumber]->myID);
		return false;
	}

	HRESULT hr = start(deviceNumber, VDList[deviceNumber]);
	if (hr == S_OK)return true;
	else return false;
}

// ----------------------------------------------------------------------
// Does both vertical buffer flipping and bgr to rgb swapping
// You have any combination of those.
// ----------------------------------------------------------------------
void videoInput::processPixels(unsigned char * src, unsigned char * dst, int width, int height, bool bRGB, bool bFlip){

	int widthInBytes = width * 3;
	int numBytes = widthInBytes * height;

	if (!bRGB){

		//int x = 0;
		//int y = 0;

		if (bFlip){
			for (int y = 0; y < height; y++){
				memcpy(dst + (y * widthInBytes), src + ((height - y - 1) * widthInBytes), widthInBytes);
			}

		}
		else{
			memcpy(dst, src, numBytes);
		}
	}
	else{
		if (bFlip){

			int x = 0;
			int y = (height - 1) * widthInBytes;
			src += y;

			for (int i = 0; i < numBytes; i += 3){
				if (x >= width){
					x = 0;
					src -= widthInBytes * 2;
				}

				*dst = *(src + 2);
				dst++;

				*dst = *(src + 1);
				dst++;

				*dst = *src;
				dst++;

				src += 3;
				x++;
			}
		}
		else{
			for (int i = 0; i < numBytes; i += 3){
				*dst = *(src + 2);
				dst++;

				*dst = *(src + 1);
				dst++;

				*dst = *src;
				dst++;

				src += 3;
			}
		}
	}
}

//------------------------------------------------------------------------------------------
void videoInput::getMediaSubtypeAsString(GUID type, char * typeAsString){
	char tmpStr[8];
	if (type == MEDIASUBTYPE_RGB24)     sprintf(tmpStr, "RGB24");
	else if (type == MEDIASUBTYPE_RGB32) sprintf(tmpStr, "RGB32");
	else if (type == MEDIASUBTYPE_RGB555)sprintf(tmpStr, "RGB555");
	else if (type == MEDIASUBTYPE_RGB565)sprintf(tmpStr, "RGB565");
	else if (type == MEDIASUBTYPE_YUY2)  sprintf(tmpStr, "YUY2");
	else if (type == MEDIASUBTYPE_YVYU)  sprintf(tmpStr, "YVYU");
	else if (type == MEDIASUBTYPE_YUYV)  sprintf(tmpStr, "YUYV");
	else if (type == MEDIASUBTYPE_IYUV)  sprintf(tmpStr, "IYUV");
	else if (type == MEDIASUBTYPE_UYVY)  sprintf(tmpStr, "UYVY");
	else if (type == MEDIASUBTYPE_YV12)  sprintf(tmpStr, "YV12");
	else if (type == MEDIASUBTYPE_YVU9)  sprintf(tmpStr, "YVU9");
	else if (type == MEDIASUBTYPE_Y411)  sprintf(tmpStr, "Y411");
	else if (type == MEDIASUBTYPE_Y41P)  sprintf(tmpStr, "Y41P");
	else if (type == MEDIASUBTYPE_Y211)  sprintf(tmpStr, "Y211");
	else if (type == MEDIASUBTYPE_AYUV)  sprintf(tmpStr, "AYUV");
	else if (type == MEDIASUBTYPE_MJPG)  sprintf(tmpStr, "MJPG");
	else if (type == MEDIASUBTYPE_Y800)  sprintf(tmpStr, "Y800");
	else if (type == MEDIASUBTYPE_Y8)    sprintf(tmpStr, "Y8");
	else if (type == MEDIASUBTYPE_GREY)  sprintf(tmpStr, "GREY");
	else if (type == MEDIASUBTYPE_I420)  sprintf(tmpStr, "I420");
	else sprintf(tmpStr, "OTHER");

	memcpy(typeAsString, tmpStr, sizeof(char)* 8);
}

int videoInput::getFourccFromMediaSubtype(GUID type) {
	return type.Data1;
}

GUID *videoInput::getMediaSubtypeFromFourcc(int fourcc){

	for (int i = 0; i<VI_NUM_TYPES; i++) {
		if ((unsigned long)(unsigned)fourcc == mediaSubtypes[i].Data1) {
			return &mediaSubtypes[i];
		}
	}

	return NULL;
}

void videoInput::getVideoPropertyAsString(int prop, char * propertyAsString){
	char tmpStr[16];

	if (prop == VideoProcAmp_Brightness) sprintf(tmpStr, "Brightness");
	else if (prop == VideoProcAmp_Contrast) sprintf(tmpStr, "Contrast");
	else if (prop == VideoProcAmp_Saturation) sprintf(tmpStr, "Saturation");
	else if (prop == VideoProcAmp_Hue) sprintf(tmpStr, "Hue");
	else if (prop == VideoProcAmp_Gain) sprintf(tmpStr, "Gain");
	else if (prop == VideoProcAmp_Gamma) sprintf(tmpStr, "Gamma");
	else if (prop == VideoProcAmp_ColorEnable) sprintf(tmpStr, "ColorEnable");
	else if (prop == VideoProcAmp_Sharpness) sprintf(tmpStr, "Sharpness");
	else sprintf(tmpStr, "%u", prop);

	memcpy(propertyAsString, tmpStr, sizeof(char)* 16);
}

int videoInput::getVideoPropertyFromCV(int cv_property){
	// see VideoProcAmpProperty in strmif.h
	switch (cv_property) {
	case CV_CAP_PROP_BRIGHTNESS:
		return VideoProcAmp_Brightness;

	case CV_CAP_PROP_CONTRAST:
		return VideoProcAmp_Contrast;

	case CV_CAP_PROP_HUE:
		return VideoProcAmp_Hue;

	case CV_CAP_PROP_SATURATION:
		return VideoProcAmp_Saturation;

	case CV_CAP_PROP_SHARPNESS:
		return VideoProcAmp_Sharpness;

	case CV_CAP_PROP_GAMMA:
		return VideoProcAmp_Gamma;

	case CV_CAP_PROP_MONOCROME:
		return VideoProcAmp_ColorEnable;

	case CV_CAP_PROP_WHITE_BALANCE_U:
		return VideoProcAmp_WhiteBalance;

	case  CV_CAP_PROP_BACKLIGHT:
		return VideoProcAmp_BacklightCompensation;

	case CV_CAP_PROP_GAIN:
		return VideoProcAmp_Gain;
	}
	return -1;
}

int videoInput::getCameraPropertyFromCV(int cv_property){
	// see CameraControlProperty in strmif.h
	switch (cv_property) {
	case CV_CAP_PROP_PAN:
		return CameraControl_Pan;

	case CV_CAP_PROP_TILT:
		return CameraControl_Tilt;

	case CV_CAP_PROP_ROLL:
		return CameraControl_Roll;

	case CV_CAP_PROP_ZOOM:
		return CameraControl_Zoom;

	case CV_CAP_PROP_EXPOSURE:
		return CameraControl_Exposure;

	case CV_CAP_PROP_IRIS:
		return CameraControl_Iris;

	case CV_CAP_PROP_FOCUS:
		return CameraControl_Focus;
	}
	return -1;
}

void videoInput::getCameraPropertyAsString(int prop, char * propertyAsString){
	char tmpStr[16];

	if (prop == CameraControl_Pan) sprintf(tmpStr, "Pan");
	else if (prop == CameraControl_Tilt) sprintf(tmpStr, "Tilt");
	else if (prop == CameraControl_Roll) sprintf(tmpStr, "Roll");
	else if (prop == CameraControl_Zoom) sprintf(tmpStr, "Zoom");
	else if (prop == CameraControl_Exposure) sprintf(tmpStr, "Exposure");
	else if (prop == CameraControl_Iris) sprintf(tmpStr, "Iris");
	else if (prop == CameraControl_Focus) sprintf(tmpStr, "Focus");
	else sprintf(tmpStr, "%u", prop);

	memcpy(propertyAsString, tmpStr, sizeof(char)* 16);
}

//-------------------------------------------------------------------------------------------
static void findClosestSizeAndSubtype(videoDevice * VD, int widthIn, int heightIn, int &widthOut, int &heightOut, GUID & mediatypeOut){
	HRESULT hr;

	//find perfect match or closest size
	int nearW = 9999999;
	int nearH = 9999999;
	//bool foundClosestMatch     = true;

	int iCount = 0;
	int iSize = 0;
	hr = VD->streamConf->GetNumberOfCapabilities(&iCount, &iSize);

	if (iSize == sizeof(VIDEO_STREAM_CONFIG_CAPS))
	{
		//For each format type RGB24 YUV2 etc
		for (int iFormat = 0; iFormat < iCount; iFormat++)
		{
			VIDEO_STREAM_CONFIG_CAPS scc;
			AM_MEDIA_TYPE *pmtConfig;
			hr = VD->streamConf->GetStreamCaps(iFormat, &pmtConfig, (BYTE*)&scc);

			if (SUCCEEDED(hr)){

				//his is how many diff sizes are available for the format
				int stepX = scc.OutputGranularityX;
				int stepY = scc.OutputGranularityY;

				int tempW = 999999;
				int tempH = 999999;

				//Don't want to get stuck in a loop
				if (stepX < 1 || stepY < 1) continue;

				//if(verbose)printf("min is %i %i max is %i %i - res is %i %i \n", scc.MinOutputSize.cx, scc.MinOutputSize.cy,  scc.MaxOutputSize.cx,  scc.MaxOutputSize.cy, stepX, stepY);
				//if(verbose)printf("min frame duration is %i  max duration is %i\n", scc.MinFrameInterval, scc.MaxFrameInterval);

				bool exactMatch = false;
				bool exactMatchX = false;
				bool exactMatchY = false;

				for (int x = scc.MinOutputSize.cx; x <= scc.MaxOutputSize.cx; x += stepX){
					//If we find an exact match
					if (widthIn == x){
						exactMatchX = true;
						tempW = x;
					}
					//Otherwise lets find the closest match based on width
					else if (abs(widthIn - x) < abs(widthIn - tempW)){
						tempW = x;
					}
				}

				for (int y = scc.MinOutputSize.cy; y <= scc.MaxOutputSize.cy; y += stepY){
					//If we find an exact match
					if (heightIn == y){
						exactMatchY = true;
						tempH = y;
					}
					//Otherwise lets find the closest match based on height
					else if (abs(heightIn - y) < abs(heightIn - tempH)){
						tempH = y;
					}
				}

				//see if we have an exact match!
				if (exactMatchX && exactMatchY){
					//foundClosestMatch = false;
					exactMatch = true;

					widthOut = widthIn;
					heightOut = heightIn;
					mediatypeOut = pmtConfig->subtype;
				}

				//otherwise lets see if this filters closest size is the closest
				//available. the closest size is determined by the sum difference
				//of the widths and heights
				else if (abs(widthIn - tempW) + abs(heightIn - tempH)  < abs(widthIn - nearW) + abs(heightIn - nearH))
				{
					nearW = tempW;
					nearH = tempH;

					widthOut = nearW;
					heightOut = nearH;
					mediatypeOut = pmtConfig->subtype;
				}

				MyDeleteMediaType(pmtConfig);

				//If we have found an exact match no need to search anymore
				if (exactMatch)break;
			}
		}
	}

}

//---------------------------------------------------------------------------------------------------
static bool setSizeAndSubtype(videoDevice * VD, int attemptWidth, int attemptHeight, GUID mediatype){
	VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(VD->pAmMediaType->pbFormat);

	//store current size
	//int tmpWidth  = HEADER(pVih)->biWidth;
	//int tmpHeight = HEADER(pVih)->biHeight;
	AM_MEDIA_TYPE * tmpType = NULL;

	HRESULT    hr = VD->streamConf->GetFormat(&tmpType);
	if (hr != S_OK)return false;

	//set new size:
	//width and height
	HEADER(pVih)->biWidth = attemptWidth;
	HEADER(pVih)->biHeight = attemptHeight;

	VD->pAmMediaType->formattype = FORMAT_VideoInfo;
	VD->pAmMediaType->majortype = MEDIATYPE_Video;
	VD->pAmMediaType->subtype = mediatype;

	//buffer size
	if (mediatype == MEDIASUBTYPE_RGB24)
	{
		VD->pAmMediaType->lSampleSize = attemptWidth*attemptHeight * 3;
	}
	else
	{
		// For compressed data, the value can be zero.
		VD->pAmMediaType->lSampleSize = 0;
	}

	//set fps if requested
	if (VD->requestedFrameTime != -1){
		pVih->AvgTimePerFrame = VD->requestedFrameTime;
	}

	//okay lets try new size
	hr = VD->streamConf->SetFormat(VD->pAmMediaType);
	if (hr == S_OK){
		if (tmpType != NULL)MyDeleteMediaType(tmpType);
		return true;
	}
	else{
		VD->streamConf->SetFormat(tmpType);
		if (tmpType != NULL)MyDeleteMediaType(tmpType);
	}

	return false;
}

// ----------------------------------------------------------------------
// Where all the work happens!
// Attempts to build a graph for the specified device
// ----------------------------------------------------------------------
int videoInput::start(int deviceID, videoDevice *VD){
	HRESULT hr = NOERROR;
	VD->myID = deviceID;
	VD->setupStarted = true;
	CAPTURE_MODE = PIN_CATEGORY_CAPTURE; //Don't worry - it ends up being preview (which is faster)
	callbackSetCount = 1;  //make sure callback method is not changed after setup called

	if (verbose)printf("SETUP: Setting up device %i\n", deviceID);

	// CREATE THE GRAPH BUILDER //
	// Create the filter graph manager and query for interfaces.
	hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void **)&VD->pCaptureGraph);
	if (FAILED(hr))    // FAILED is a macro that tests the return value
	{
		if (verbose)printf("ERROR - Could not create the Filter Graph Manager\n");
		return hr;
	}

	//FITLER GRAPH MANAGER//
	// Create the Filter Graph Manager.
	hr = CoCreateInstance(CLSID_FilterGraph, 0, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&VD->pGraph);
	if (FAILED(hr))
	{
		if (verbose)printf("ERROR - Could not add the graph builder!\n");
		stopDevice(deviceID);
		return hr;
	}

	//SET THE FILTERGRAPH//
	hr = VD->pCaptureGraph->SetFiltergraph(VD->pGraph);
	if (FAILED(hr))
	{
		if (verbose)printf("ERROR - Could not set filtergraph\n");
		stopDevice(deviceID);
		return hr;
	}

	//MEDIA CONTROL (START/STOPS STREAM)//
	// Using QueryInterface on the graph builder,
	// Get the Media Control object.
	hr = VD->pGraph->QueryInterface(IID_IMediaControl, (void **)&VD->pControl);
	if (FAILED(hr))
	{
		if (verbose)printf("ERROR - Could not create the Media Control object\n");
		stopDevice(deviceID);
		return hr;
	}

	//FIND VIDEO DEVICE AND ADD TO GRAPH//
	//gets the device specified by the second argument.
	hr = getDevice(&VD->pVideoInputFilter, deviceID, VD->wDeviceName, VD->nDeviceName);

	if (SUCCEEDED(hr)){
		if (verbose)printf("SETUP: %s\n", VD->nDeviceName);
		hr = VD->pGraph->AddFilter(VD->pVideoInputFilter, VD->wDeviceName);
	}
	else{
		if (verbose)printf("ERROR - Could not find specified video device\n");
		stopDevice(deviceID);
		return hr;
	}

	//LOOK FOR PREVIEW PIN IF THERE IS NONE THEN WE USE CAPTURE PIN AND THEN SMART TEE TO PREVIEW
	IAMStreamConfig *streamConfTest = NULL;
	hr = VD->pCaptureGraph->FindInterface(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, VD->pVideoInputFilter, IID_IAMStreamConfig, (void **)&streamConfTest);
	if (FAILED(hr)){
		if (verbose)printf("SETUP: Couldn't find preview pin using SmartTee\n");
	}
	else{
		CAPTURE_MODE = PIN_CATEGORY_PREVIEW;
		streamConfTest->Release();
		streamConfTest = NULL;
	}

	//CROSSBAR (SELECT PHYSICAL INPUT TYPE)//
	//my own function that checks to see if the device can support a crossbar and if so it routes it.
	//webcams tend not to have a crossbar so this function will also detect a webcams and not apply the crossbar
	if (VD->useCrossbar)
	{
		if (verbose)printf("SETUP: Checking crossbar\n");
		routeCrossbar(&VD->pCaptureGraph, &VD->pVideoInputFilter, VD->connection, CAPTURE_MODE);
	}

	//we do this because webcams don't have a preview mode
	hr = VD->pCaptureGraph->FindInterface(&CAPTURE_MODE, &MEDIATYPE_Video, VD->pVideoInputFilter, IID_IAMStreamConfig, (void **)&VD->streamConf);
	if (FAILED(hr)){
		if (verbose)printf("ERROR: Couldn't config the stream!\n");
		stopDevice(deviceID);
		return hr;
	}

	//NOW LETS DEAL WITH GETTING THE RIGHT SIZE
	hr = VD->streamConf->GetFormat(&VD->pAmMediaType);
	if (FAILED(hr)){
		if (verbose)printf("ERROR: Couldn't getFormat for pAmMediaType!\n");
		stopDevice(deviceID);
		return hr;
	}

	VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(VD->pAmMediaType->pbFormat);
	int currentWidth = HEADER(pVih)->biWidth;
	int currentHeight = HEADER(pVih)->biHeight;

	bool customSize = VD->tryDiffSize;

	bool foundSize = false;

	if (customSize){
		if (verbose)    printf("SETUP: Default Format is set to %i by %i \n", currentWidth, currentHeight);

		char guidStr[8];
		// try specified format and size
		getMediaSubtypeAsString(VD->tryVideoType, guidStr);
		if (verbose)printf("SETUP: trying specified format %s @ %i by %i\n", guidStr, VD->tryWidth, VD->tryHeight);

		if (setSizeAndSubtype(VD, VD->tryWidth, VD->tryHeight, VD->tryVideoType)){
			VD->setSize(VD->tryWidth, VD->tryHeight);
			VD->videoType = VD->tryVideoType;
			foundSize = true;
		}
		else {
			// try specified size with all formats
			for (int i = 0; i < VI_NUM_TYPES; i++){

				getMediaSubtypeAsString(mediaSubtypes[i], guidStr);

				if (verbose)printf("SETUP: trying format %s @ %i by %i\n", guidStr, VD->tryWidth, VD->tryHeight);
				if (setSizeAndSubtype(VD, VD->tryWidth, VD->tryHeight, mediaSubtypes[i])){
					VD->setSize(VD->tryWidth, VD->tryHeight);
					VD->videoType = mediaSubtypes[i];
					foundSize = true;
					break;
				}
			}
		}

		//if we didn't find the requested size - lets try and find the closest matching size
		if (foundSize == false){
			if (verbose)printf("SETUP: couldn't find requested size - searching for closest matching size\n");

			int closestWidth = -1;
			int closestHeight = -1;
			GUID newMediaSubtype;

			findClosestSizeAndSubtype(VD, VD->tryWidth, VD->tryHeight, closestWidth, closestHeight, newMediaSubtype);

			if (closestWidth != -1 && closestHeight != -1){
				getMediaSubtypeAsString(newMediaSubtype, guidStr);

				if (verbose)printf("SETUP: closest supported size is %s @ %i %i\n", guidStr, closestWidth, closestHeight);
				if (setSizeAndSubtype(VD, closestWidth, closestHeight, newMediaSubtype)){
					VD->setSize(closestWidth, closestHeight);
					foundSize = true;
				}
			}
		}
	}

	//if we didn't specify a custom size or if we did but couldn't find it lets setup with the default settings
	if (customSize == false || foundSize == false){
		if (VD->requestedFrameTime != -1){
			pVih->AvgTimePerFrame = VD->requestedFrameTime;
			hr = VD->streamConf->SetFormat(VD->pAmMediaType);
		}
		VD->setSize(currentWidth, currentHeight);
	}

	//SAMPLE GRABBER (ALLOWS US TO GRAB THE BUFFER)//
	// Create the Sample Grabber.
	hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&VD->pGrabberF);
	if (FAILED(hr)){
		if (verbose)printf("Could not Create Sample Grabber - CoCreateInstance()\n");
		stopDevice(deviceID);
		return hr;
	}

	hr = VD->pGraph->AddFilter(VD->pGrabberF, L"Sample Grabber");
	if (FAILED(hr)){
		if (verbose)printf("Could not add Sample Grabber - AddFilter()\n");
		stopDevice(deviceID);
		return hr;
	}

	hr = VD->pGrabberF->QueryInterface(IID_ISampleGrabber, (void**)&VD->pGrabber);
	if (FAILED(hr)){
		if (verbose)printf("ERROR: Could not query SampleGrabber\n");
		stopDevice(deviceID);
		return hr;
	}

	//Set Params - One Shot should be false unless you want to capture just one buffer
	hr = VD->pGrabber->SetOneShot(FALSE);
	if (bCallback){
		hr = VD->pGrabber->SetBufferSamples(FALSE);
	}
	else{
		hr = VD->pGrabber->SetBufferSamples(TRUE);
	}

	if (bCallback){
		//Tell the grabber to use our callback function - 0 is for SampleCB and 1 for BufferCB
		//We use SampleCB
		hr = VD->pGrabber->SetCallback(VD->sgCallback, 0);
		if (FAILED(hr)){
			if (verbose)printf("ERROR: problem setting callback\n");
			stopDevice(deviceID);
			return hr;
		}
		else{
			if (verbose)printf("SETUP: Capture callback set\n");
		}
	}

	//MEDIA CONVERSION
	//Get video properties from the stream's mediatype and apply to the grabber (otherwise we don't get an RGB image)
	//zero the media type - lets try this :) - maybe this works?
	AM_MEDIA_TYPE mt;
	ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));

	mt.majortype = MEDIATYPE_Video;
	mt.subtype = MEDIASUBTYPE_RGB24;
	mt.formattype = FORMAT_VideoInfo;

	//VD->pAmMediaType->subtype = VD->videoType;
	hr = VD->pGrabber->SetMediaType(&mt);

	//lets try freeing our stream conf here too
	//this will fail if the device is already running
	if (VD->streamConf){
		VD->streamConf->Release();
		VD->streamConf = NULL;
	}
	else{
		if (verbose)printf("ERROR: connecting device - prehaps it is already being used?\n");
		stopDevice(deviceID);
		return S_FALSE;
	}

	//NULL RENDERER//
	//used to give the video stream somewhere to go to.
	hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)(&VD->pDestFilter));
	if (FAILED(hr)){
		if (verbose)printf("ERROR: Could not create filter - NullRenderer\n");
		stopDevice(deviceID);
		return hr;
	}

	hr = VD->pGraph->AddFilter(VD->pDestFilter, L"NullRenderer");
	if (FAILED(hr)){
		if (verbose)printf("ERROR: Could not add filter - NullRenderer\n");
		stopDevice(deviceID);
		return hr;
	}

	//RENDER STREAM//
	//This is where the stream gets put together.
	hr = VD->pCaptureGraph->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, VD->pVideoInputFilter, VD->pGrabberF, VD->pDestFilter);

	if (FAILED(hr)){
		if (verbose)printf("ERROR: Could not connect pins - RenderStream()\n");
		stopDevice(deviceID);
		return hr;
	}

	//EXP - lets try setting the sync source to null - and make it run as fast as possible
	{
		IMediaFilter *pMediaFilter = 0;
		hr = VD->pGraph->QueryInterface(IID_IMediaFilter, (void**)&pMediaFilter);
		if (FAILED(hr)){
			if (verbose)printf("ERROR: Could not get IID_IMediaFilter interface\n");
		}
		else{
			pMediaFilter->SetSyncSource(NULL);
			pMediaFilter->Release();
		}
	}

	//LETS RUN THE STREAM!
	hr = VD->pControl->Run();

	if (FAILED(hr)){
		if (verbose)printf("ERROR: Could not start graph\n");
		stopDevice(deviceID);
		return hr;
	}

	//MAKE SURE THE DEVICE IS SENDING VIDEO BEFORE WE FINISH
	if (!bCallback){

		long bufferSize = VD->videoSize;

		while (hr != S_OK){
			hr = VD->pGrabber->GetCurrentBuffer(&bufferSize, (long *)VD->pBuffer);
			Sleep(10);
		}

	}

	if (verbose)printf("SETUP: Device is setup and ready to capture.\n\n");
	VD->readyToCapture = true;

	//Release filters - seen someone else do this
	//looks like it solved the freezes

	//if we release this then we don't have access to the settings
	//we release our video input filter but then reconnect with it
	//each time we need to use it
	VD->pVideoInputFilter->Release();
	VD->pVideoInputFilter = NULL;

	VD->pGrabberF->Release();
	VD->pGrabberF = NULL;

	VD->pDestFilter->Release();
	VD->pDestFilter = NULL;

	return S_OK;
}

// ----------------------------------------------------------------------
// Returns number of good devices
//
// ----------------------------------------------------------------------
int videoInput::getDeviceCount(){
	ICreateDevEnum *pDevEnum = NULL;
	IEnumMoniker *pEnum = NULL;
	int deviceCounter = 0;

	HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL,
		CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
		reinterpret_cast<void**>(&pDevEnum));

	if (SUCCEEDED(hr))
	{
		// Create an enumerator for the video capture category.
		hr = pDevEnum->CreateClassEnumerator(
			CLSID_VideoInputDeviceCategory,
			&pEnum, 0);

		if (hr == S_OK){
			IMoniker *pMoniker = NULL;
			while (pEnum->Next(1, &pMoniker, NULL) == S_OK){

				IPropertyBag *pPropBag;
				hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag,
					(void**)(&pPropBag));

				if (FAILED(hr)){
					pMoniker->Release();
					continue;  // Skip this one, maybe the next one will work.
				}

				pPropBag->Release();
				pPropBag = NULL;

				pMoniker->Release();
				pMoniker = NULL;

				deviceCounter++;
			}

			pEnum->Release();
			pEnum = NULL;
		}

		pDevEnum->Release();
		pDevEnum = NULL;
	}
	return deviceCounter;
}

// ----------------------------------------------------------------------
// Do we need this?
//
// Enumerate all of the video input devices
// Return the filter with a matching friendly name
// ----------------------------------------------------------------------
HRESULT videoInput::getDevice(IBaseFilter** gottaFilter, int deviceId, WCHAR * wDeviceName, char * nDeviceName){
	BOOL done = false;
	int deviceCounter = 0;

	// Create the System Device Enumerator.
	ICreateDevEnum *pSysDevEnum = NULL;
	HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void **)&pSysDevEnum);
	if (FAILED(hr))
	{
		return hr;
	}

	// Obtain a class enumerator for the video input category.
	IEnumMoniker *pEnumCat = NULL;
	hr = pSysDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnumCat, 0);

	if (hr == S_OK)
	{
		// Enumerate the monikers.
		IMoniker *pMoniker = NULL;
		ULONG cFetched;
		while ((pEnumCat->Next(1, &pMoniker, &cFetched) == S_OK) && (!done))
		{
			if (deviceCounter == deviceId)
			{
				// Bind the first moniker to an object
				IPropertyBag *pPropBag;
				hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&pPropBag);
				if (SUCCEEDED(hr))
				{
					// To retrieve the filter's friendly name, do the following:
					VARIANT varName;
					VariantInit(&varName);
					hr = pPropBag->Read(L"FriendlyName", &varName, 0);
					if (SUCCEEDED(hr))
					{

						//copy the name to nDeviceName & wDeviceName
						int count = 0;
						while (varName.bstrVal[count] != 0x00) {
							wDeviceName[count] = varName.bstrVal[count];
							nDeviceName[count] = (char)varName.bstrVal[count];
							count++;
						}

						// We found it, so send it back to the caller
						hr = pMoniker->BindToObject(NULL, NULL, IID_IBaseFilter, (void**)gottaFilter);
						done = true;
					}
					VariantClear(&varName);
					pPropBag->Release();
					pPropBag = NULL;
					pMoniker->Release();
					pMoniker = NULL;
				}
			}
			deviceCounter++;
		}
		pEnumCat->Release();
		pEnumCat = NULL;
	}
	pSysDevEnum->Release();
	pSysDevEnum = NULL;

	if (done) {
		return hr;    // found it, return native error
	}
	else {
		return VFW_E_NOT_FOUND;    // didn't find it error
	}
}

// ----------------------------------------------------------------------
// Show the property pages for a filter
// This is stolen from the DX9 SDK
// ----------------------------------------------------------------------
HRESULT videoInput::ShowFilterPropertyPages(IBaseFilter *pFilter){
	ISpecifyPropertyPages *pProp;

	HRESULT hr = pFilter->QueryInterface(IID_ISpecifyPropertyPages, (void **)&pProp);
	if (SUCCEEDED(hr))
	{
		// Get the filter's name and IUnknown pointer.
		FILTER_INFO FilterInfo;
		hr = pFilter->QueryFilterInfo(&FilterInfo);
		IUnknown *pFilterUnk;
		pFilter->QueryInterface(IID_IUnknown, (void **)&pFilterUnk);

		// Show the page.
		CAUUID caGUID;
		pProp->GetPages(&caGUID);
		pProp->Release();
		OleCreatePropertyFrame(
			NULL,                   // Parent window
			0, 0,                   // Reserved
			FilterInfo.achName,     // Caption for the dialog box
			1,                      // Number of objects (just the filter)
			&pFilterUnk,            // Array of object pointers.
			caGUID.cElems,          // Number of property pages
			caGUID.pElems,          // Array of property page CLSIDs
			0,                      // Locale identifier
			0, NULL                 // Reserved
			);

		// Clean up.
		if (pFilterUnk)pFilterUnk->Release();
		if (FilterInfo.pGraph)FilterInfo.pGraph->Release();
		CoTaskMemFree(caGUID.pElems);
	}
	return hr;
}

HRESULT videoInput::ShowStreamPropertyPages(IAMStreamConfig  * /*pStream*/){

	HRESULT hr = NOERROR;
	return hr;
}

// ----------------------------------------------------------------------
// This code was also brazenly stolen from the DX9 SDK
// Pass it a file name in wszPath, and it will save the filter graph to that file.
// ----------------------------------------------------------------------
HRESULT videoInput::SaveGraphFile(IGraphBuilder *pGraph, WCHAR *wszPath) {
	const WCHAR wszStreamName[] = L"ActiveMovieGraph";
	HRESULT hr;
	IStorage *pStorage = NULL;

	// First, create a document file which will hold the GRF file
	hr = StgCreateDocfile(
		wszPath,
		STGM_CREATE | STGM_TRANSACTED | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
		0, &pStorage);
	if (FAILED(hr))
	{
		return hr;
	}

	// Next, create a stream to store.
	IStream *pStream;
	hr = pStorage->CreateStream(
		wszStreamName,
		STGM_WRITE | STGM_CREATE | STGM_SHARE_EXCLUSIVE,
		0, 0, &pStream);
	if (FAILED(hr))
	{
		pStorage->Release();
		return hr;
	}

	// The IPersistStream converts a stream into a persistent object.
	IPersistStream *pPersist = NULL;
	pGraph->QueryInterface(IID_IPersistStream, reinterpret_cast<void**>(&pPersist));
	hr = pPersist->Save(pStream, TRUE);
	pStream->Release();
	pPersist->Release();
	if (SUCCEEDED(hr))
	{
		hr = pStorage->Commit(STGC_DEFAULT);
	}
	pStorage->Release();
	return hr;
}

// ----------------------------------------------------------------------
// For changing the input types
//
// ----------------------------------------------------------------------
HRESULT videoInput::routeCrossbar(ICaptureGraphBuilder2 **ppBuild, IBaseFilter **pVidInFilter, int conType, GUID captureMode){

	//create local ICaptureGraphBuilder2
	ICaptureGraphBuilder2 *pBuild = NULL;
	pBuild = *ppBuild;

	//create local IBaseFilter
	IBaseFilter *pVidFilter = NULL;
	pVidFilter = *pVidInFilter;

	// Search upstream for a crossbar.
	IAMCrossbar *pXBar1 = NULL;
	HRESULT hr = pBuild->FindInterface(&LOOK_UPSTREAM_ONLY, NULL, pVidFilter,
		IID_IAMCrossbar, (void**)&pXBar1);
	if (SUCCEEDED(hr))
	{

		bool foundDevice = false;

		if (verbose)printf("SETUP: You are not a webcam! Setting Crossbar\n");
		pXBar1->Release();

		IAMCrossbar *Crossbar;
		hr = pBuild->FindInterface(&captureMode, &MEDIATYPE_Interleaved, pVidFilter, IID_IAMCrossbar, (void **)&Crossbar);

		if (hr != NOERROR){
			hr = pBuild->FindInterface(&captureMode, &MEDIATYPE_Video, pVidFilter, IID_IAMCrossbar, (void **)&Crossbar);
		}

		LONG lInpin, lOutpin;
		hr = Crossbar->get_PinCounts(&lOutpin, &lInpin);

		BOOL iPin = TRUE; LONG pIndex = 0, pRIndex = 0, pType = 0;

		while (pIndex < lInpin)
		{
			hr = Crossbar->get_CrossbarPinInfo(iPin, pIndex, &pRIndex, &pType);

			if (pType == conType){
				if (verbose)printf("SETUP: Found Physical Interface");

				switch (conType){

				case PhysConn_Video_Composite:
					if (verbose)printf(" - Composite\n");
					break;
				case PhysConn_Video_SVideo:
					if (verbose)printf(" - S-Video\n");
					break;
				case PhysConn_Video_Tuner:
					if (verbose)printf(" - Tuner\n");
					break;
				case PhysConn_Video_USB:
					if (verbose)printf(" - USB\n");
					break;
				case PhysConn_Video_1394:
					if (verbose)printf(" - Firewire\n");
					break;
				}

				foundDevice = true;
				break;
			}
			pIndex++;

		}

		if (foundDevice){
			BOOL OPin = FALSE; LONG pOIndex = 0, pORIndex = 0, pOType = 0;
			while (pOIndex < lOutpin)
			{
				hr = Crossbar->get_CrossbarPinInfo(OPin, pOIndex, &pORIndex, &pOType);
				if (pOType == PhysConn_Video_VideoDecoder)
					break;
			}
			Crossbar->Route(pOIndex, pIndex);
		}
		else{
			if (verbose) printf("SETUP: Didn't find specified Physical Connection type. Using Defualt. \n");
		}

		//we only free the crossbar when we close or restart the device
		//we were getting a crash otherwise
		//if(Crossbar)Crossbar->Release();
		//if(Crossbar)Crossbar = NULL;

		if (pXBar1)pXBar1->Release();
		if (pXBar1)pXBar1 = NULL;

	}
	else{
		if (verbose) printf("SETUP: You are a webcam or snazzy firewire cam! No Crossbar needed\n");
		return hr;
	}

	return hr;
}

/********************* Capturing video from camera via DirectShow *********************/
struct SuppressVideoInputMessages
{
	SuppressVideoInputMessages() { videoInput::setVerbose(false); }
};

static SuppressVideoInputMessages do_it;
videoInput CvCaptureCAM_DShow::VI;

CvCaptureCAM_DShow::CvCaptureCAM_DShow()
{
	index = -1;
	frame = 0;
	width = height = fourcc = -1;
	widthSet = heightSet = -1;
	CoInitialize(0);
}

CvCaptureCAM_DShow::~CvCaptureCAM_DShow()
{
	close();
	CoUninitialize();
}

void CvCaptureCAM_DShow::close()
{
	if (index >= 0)
	{
		VI.stopDevice(index);
		index = -1;
		cvReleaseImage(&frame);
	}
	widthSet = heightSet = width = height = -1;
}

// Initialize camera input
bool CvCaptureCAM_DShow::open(int _index)
{
	int devices = 0;

	close();
	devices = VI.listDevices(true);
	if (devices == 0)
		return false;
	if (_index < 0 || _index > devices - 1)
		return false;
	VI.setupDevice(_index);
	if (!VI.isDeviceSetup(_index))
		return false;
	index = _index;
	return true;
}

bool CvCaptureCAM_DShow::grabFrame()
{
	return true;
}

IplImage* CvCaptureCAM_DShow::retrieveFrame(int)
{
	if (!frame || VI.getWidth(index) != frame->width || VI.getHeight(index) != frame->height)
	{
		if (frame)
			cvReleaseImage(&frame);
		int w = VI.getWidth(index), h = VI.getHeight(index);
		frame = cvCreateImage(cvSize(w, h), 8, 3);
	}

	if (VI.getPixels(index, (uchar*)frame->imageData, false, true))
		return frame;
	else
		return NULL;
}

double CvCaptureCAM_DShow::getProperty(int property_id)
{

	long min_value, max_value, stepping_delta, current_value, flags, defaultValue;

	// image format proprrties
	switch (property_id)
	{
	case CV_CAP_PROP_FRAME_WIDTH:
		return VI.getWidth(index);

	case CV_CAP_PROP_FRAME_HEIGHT:
		return VI.getHeight(index);

	case CV_CAP_PROP_FOURCC:
		return VI.getFourcc(index);

	case CV_CAP_PROP_FPS:
		return VI.getFPS(index);
	}

	// video filter properties
	switch (property_id)
	{
	case CV_CAP_PROP_BRIGHTNESS:
	case CV_CAP_PROP_CONTRAST:
	case CV_CAP_PROP_HUE:
	case CV_CAP_PROP_SATURATION:
	case CV_CAP_PROP_SHARPNESS:
	case CV_CAP_PROP_GAMMA:
	case CV_CAP_PROP_MONOCROME:
	case CV_CAP_PROP_WHITE_BALANCE_U:
	case CV_CAP_PROP_BACKLIGHT:
	case CV_CAP_PROP_GAIN:
		if (VI.getVideoSettingFilter(index, VI.getVideoPropertyFromCV(property_id), min_value, max_value, stepping_delta, current_value, flags, defaultValue)) return (double)current_value;
	}

	// camera properties
	switch (property_id)
	{
	case CV_CAP_PROP_PAN:
	case CV_CAP_PROP_TILT:
	case CV_CAP_PROP_ROLL:
	case CV_CAP_PROP_ZOOM:
	case CV_CAP_PROP_EXPOSURE:
	case CV_CAP_PROP_IRIS:
	case CV_CAP_PROP_FOCUS:
		if (VI.getVideoSettingCamera(index, VI.getCameraPropertyFromCV(property_id), min_value, max_value, stepping_delta, current_value, flags, defaultValue)) return (double)current_value;

	}

	// unknown parameter or value not available
	return -1;
}

bool CvCaptureCAM_DShow::setProperty(int property_id, double value)
{
	// image capture properties
	bool handled = false;
	switch (property_id)
	{
	case CV_CAP_PROP_FRAME_WIDTH:
		width = cvRound(value);
		handled = true;
		break;

	case CV_CAP_PROP_FRAME_HEIGHT:
		height = cvRound(value);
		handled = true;
		break;

	case CV_CAP_PROP_FOURCC:
		fourcc = (int)(unsigned long)(value);
		if (fourcc == -1) {
			// following cvCreateVideo usage will pop up caprturepindialog here if fourcc=-1
			// TODO - how to create a capture pin dialog
		}
		handled = true;
		break;

	case CV_CAP_PROP_FPS:
		int fps = cvRound(value);
		if (fps != VI.getFPS(index))
		{
			VI.stopDevice(index);
			VI.setIdealFramerate(index, fps);
			if (widthSet > 0 && heightSet > 0)
				VI.setupDevice(index, widthSet, heightSet);
			else
				VI.setupDevice(index);
		}
		return VI.isDeviceSetup(index);

	}

	if (handled) {
		// a stream setting
		if (width > 0 && height > 0)
		{
			if (width != VI.getWidth(index) || height != VI.getHeight(index))//|| fourcc != VI.getFourcc(index) )
			{
				int fps = static_cast<int>(VI.getFPS(index));
				VI.stopDevice(index);
				VI.setIdealFramerate(index, fps);
				VI.setupDeviceFourcc(index, width, height, fourcc);
			}

			bool success = VI.isDeviceSetup(index);
			if (success)
			{
				widthSet = width;
				heightSet = height;
				width = height = fourcc = -1;
			}
			return success;
		}
		return true;
	}

	// show video/camera filter dialog
	if (property_id == CV_CAP_PROP_SETTINGS) {
		VI.showSettingsWindow(index);
		return true;
	}

	//video Filter properties
	switch (property_id)
	{
	case CV_CAP_PROP_BRIGHTNESS:
	case CV_CAP_PROP_CONTRAST:
	case CV_CAP_PROP_HUE:
	case CV_CAP_PROP_SATURATION:
	case CV_CAP_PROP_SHARPNESS:
	case CV_CAP_PROP_GAMMA:
	case CV_CAP_PROP_MONOCROME:
	case CV_CAP_PROP_WHITE_BALANCE_U:
	case CV_CAP_PROP_BACKLIGHT:
	case CV_CAP_PROP_GAIN:
		return VI.setVideoSettingFilter(index, VI.getVideoPropertyFromCV(property_id), (long)value);
	}

	//camera properties
	switch (property_id)
	{
	case CV_CAP_PROP_PAN:
	case CV_CAP_PROP_TILT:
	case CV_CAP_PROP_ROLL:
	case CV_CAP_PROP_ZOOM:
	case CV_CAP_PROP_EXPOSURE:
	case CV_CAP_PROP_IRIS:
	case CV_CAP_PROP_FOCUS:
		return VI.setVideoSettingCamera(index, VI.getCameraPropertyFromCV(property_id), (long)value);
	}

	return false;
}

bool CvCaptureCAM_DShow::getDevicesList(std::map<int, std::string>& filenames) const
{
	filenames.clear();

	for (int i = 0; i < VI.getDeviceCount(); ++i) {
		filenames[i] = VI.getDeviceName(i);
	}

	return true;
}

CvCapture* cvCreateCameraCapture_DShow(int index)
{
	CvCaptureCAM_DShow* capture = new CvCaptureCAM_DShow;

	try {
		if (capture->open(index))
			return capture;
	} catch (...) {
		delete capture;
		throw;
	}

	delete capture;
	return nullptr;
}

} // namespace fbc

#endif // _MSC_VER
