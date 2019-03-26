/*********************************************************************
 * MIDIOutDevice.cpp - Implementation for CMIDIOutDevice and related 
 *                     classes.
 *
 * Written by: Leslie Sanford (jabberdabber@hotmail.com)
 *
 * Last modified: 08/13/2002
 ********************************************************************/


//--------------------------------------------------------------------
// Dependencies
//--------------------------------------------------------------------


#include "MIDIOutDevice.h"
#include "midi.h"


//--------------------------------------------------------------------
// Using declarations
//--------------------------------------------------------------------


using midi::CMIDIOutDevice;
using midi::CMIDIOutException;


//--------------------------------------------------------------------
// CMIDIOutHeader implementation
//--------------------------------------------------------------------


// Constructor
CMIDIOutDevice::CMIDIOutHeader::CMIDIOutHeader(HMIDIOUT DevHandle,
                                               LPSTR Msg, 
                                               DWORD MsgLength) :
m_DevHandle(DevHandle)
{
    // Initialize header
    m_MIDIHdr.lpData         = Msg;
    m_MIDIHdr.dwBufferLength = MsgLength;
    m_MIDIHdr.dwFlags        = 0;

    // Prepare header
    MMRESULT Result = ::midiOutPrepareHeader(DevHandle, &m_MIDIHdr,
                                             sizeof m_MIDIHdr);

    // If an error occurred, throw exception
    if(Result != MMSYSERR_NOERROR)
    {
        throw CMIDIOutException(Result);
    }
}


// Destructor
CMIDIOutDevice::CMIDIOutHeader::~CMIDIOutHeader()
{
    ::midiOutUnprepareHeader(m_DevHandle, &m_MIDIHdr, 
                             sizeof m_MIDIHdr);
}


// Sends long message
void CMIDIOutDevice::CMIDIOutHeader::SendMsg()
{
    MMRESULT Result = ::midiOutLongMsg(m_DevHandle, &m_MIDIHdr,
                                       sizeof m_MIDIHdr);

    if(Result != MMSYSERR_NOERROR)
    {
        throw CMIDIOutException(Result);
    }
}


//--------------------------------------------------------------------
// CHeaderQueue implementation
//--------------------------------------------------------------------


// Constructor
CMIDIOutDevice::CHeaderQueue::CHeaderQueue()
{
    ::InitializeCriticalSection(&m_CriticalSection);
}


// Destructor
CMIDIOutDevice::CHeaderQueue::~CHeaderQueue()
{
    RemoveAll();

    ::DeleteCriticalSection(&m_CriticalSection);
}


// Add header to queue
void CMIDIOutDevice::CHeaderQueue::AddHeader(
                               CMIDIOutDevice::CMIDIOutHeader *Header)
{
    ::EnterCriticalSection(&m_CriticalSection);

    m_HdrQueue.push(Header);

    ::LeaveCriticalSection(&m_CriticalSection);
}


// Remove header from queue
void CMIDIOutDevice::CHeaderQueue::RemoveHeader()
{
    ::EnterCriticalSection(&m_CriticalSection);

    if(!m_HdrQueue.empty())
    {
        delete m_HdrQueue.front();
        m_HdrQueue.pop();
    }

    ::LeaveCriticalSection(&m_CriticalSection);
}


// Empty header queue
void CMIDIOutDevice::CHeaderQueue::RemoveAll()
{
    ::EnterCriticalSection(&m_CriticalSection);

    while(!m_HdrQueue.empty())
    {
        delete m_HdrQueue.front();
        m_HdrQueue.pop();
    }

    ::LeaveCriticalSection(&m_CriticalSection);
}


// Determines if the header queue is empty
bool CMIDIOutDevice::CHeaderQueue::IsEmpty()
{
    bool Result;

    ::EnterCriticalSection(&m_CriticalSection);

    Result = m_HdrQueue.empty();

    ::LeaveCriticalSection(&m_CriticalSection);

    return Result;
}


//--------------------------------------------------------------------
// CMIDIOutDevice implementation
//--------------------------------------------------------------------


// Constructs CMIDIOutDevice object in an closed state
CMIDIOutDevice::CMIDIOutDevice() :
m_State(CLOSED)
{
    // If we are unable to create signalling event, throw exception
    if(!CreateEvent())
    {
        throw CMIDIOutEventFailure();
    }
}


// Constructs CMIDIOutDevice object in an opened state
CMIDIOutDevice::CMIDIOutDevice(UINT DeviceId) :
m_State(CLOSED)
{
    // Open device
    Open(DeviceId);

    // If we are unable to create signalling event, throw exception
    if(!CreateEvent())
    {
        Close();
        throw CMIDIOutEventFailure();
    }
}


// Destruction
CMIDIOutDevice::~CMIDIOutDevice()
{
    // Close device
    Close();

    // Close handle to signalling event
    ::CloseHandle(m_Event);
}


// Opens the MIDI output device
void CMIDIOutDevice::Open(UINT DeviceId)
{
    // Makes sure the previous device, if any, is closed before 
    // opening another one
    Close();

    DWORD Dummy;

    // If we are unable to create worker thread for handling the 
    // headers, throw exception
    if((::CreateThread(NULL, 0, HeaderProc, this, 0, &Dummy)) == NULL)
    {
        throw CMIDIOutThreadFailure();
    }

    // Open MIDI output device
    MMRESULT Result = ::midiOutOpen(&m_DevHandle, DeviceId, 
                                 reinterpret_cast<DWORD>(MidiOutProc),
                                 reinterpret_cast<DWORD>(this),
                                 CALLBACK_FUNCTION);

    // If we are able to open the device, change state
    if(Result == MMSYSERR_NOERROR)
    {
        m_State = OPENED;
    }
    // Else opening failed, throw exception
    else
    {
        ::SetEvent(m_Event);
        throw CMIDIOutException(Result);
    }
}


// Closes the MIDI output device
void CMIDIOutDevice::Close()
{
    // Only close an already opened device
    if(m_State == OPENED)
    {
        // Change state
        m_State = CLOSED;

        // Notify our worker thread to finish
        ::SetEvent(m_Event);

        // Empty header queue - we're finished with the headers
        m_HdrQueue.RemoveAll();

        // Close the MIDI output device
        ::midiOutClose(m_DevHandle);
    }
}


// Sends short message
void CMIDIOutDevice::SendMsg(DWORD Msg)
{
    if(m_State == OPENED)
    {
        MMRESULT Result = ::midiOutShortMsg(m_DevHandle, Msg);

        if(Result != MMSYSERR_NOERROR)
        {
            throw CMIDIOutException(Result);
        }
    }
}


// Sends long message
void CMIDIOutDevice::SendMsg(LPSTR Msg, DWORD MsgLength)
{
    if(m_State == OPENED)
    {  
        CMIDIOutHeader *Header;

        try
        {
            // Create new header to send system exclusive message
            Header = new CMIDIOutHeader(m_DevHandle, Msg, MsgLength);
        }
        // If memory allocation failed, throw exception
        catch(const std::bad_alloc &)
        {
            throw CMIDIOutMemFailure();
        }
        // If preparing the header failed, rethrow exception
        catch(const CMIDIOutException &)
        {
            throw;
        }

        try
        {
            // Send system exclusive data
            Header->SendMsg();

            // Add header to queue
            m_HdrQueue.AddHeader(Header);
        }
        // If sending system exclusive msg failed, release header
        // and rethrow exception
        catch(const CMIDIOutException &)
        {
            delete Header;
        }
    }
}


// Determines if the MIDI output device is opened
bool CMIDIOutDevice::IsOpen() const
{
    return (m_State == OPENED);
}


// Gets the capabilities of a particular MIDI output device
void CMIDIOutDevice::GetDevCaps(UINT DeviceId, MIDIOUTCAPS &Caps)
{
    MMRESULT Result = ::midiOutGetDevCaps(DeviceId, &Caps, 
                                          sizeof Caps);

    // If we are not able to retrieve device capabilities, throw 
    // exception
    if(Result != MMSYSERR_NOERROR)
    {
        throw CMIDIOutException(Result);
    }
}


// Packs data into short message
void CMIDIOutDevice::PackShortMsg(DWORD &Msg, unsigned char Status,
                                  unsigned char DataByte1,
                                  unsigned char DataByte2)
{
    Msg = Status;
    Msg |= DataByte1 << midi::SHORT_MSG_SHIFT;
    Msg |= DataByte2 << midi::SHORT_MSG_SHIFT;
}


// Packs data into short channel message
void CMIDIOutDevice::PackShortMsg(DWORD &Msg, unsigned char Command,
                                  unsigned char Channel,
                                  unsigned char DataByte1,
                                  unsigned char DataByte2)
{
    Msg = Command | Channel;
    Msg |= DataByte1 << midi::SHORT_MSG_SHIFT;
    Msg |= DataByte2 << midi::SHORT_MSG_SHIFT * 2;
}


// Creates event for signalling header thread
bool CMIDIOutDevice::CreateEvent()
{
    bool Result = true;

    m_Event = ::CreateEvent(NULL, FALSE, FALSE, NULL);

    // If event creation failed, record failure
    if(m_Event == NULL)
    {
        Result = false;
    }

    return Result;
}

// Called by Windows when a MIDI input event occurs
void CALLBACK CMIDIOutDevice::MidiOutProc(HMIDIOUT MidiOut, UINT Msg,
                                          DWORD Instance, DWORD Param1,
                                          DWORD Param2)
{
    CMIDIOutDevice *Device;
    
    Device = reinterpret_cast<CMIDIOutDevice *>(Instance);

    if(Msg == MOM_DONE)
    {
        ::SetEvent(Device->m_Event);
    }
}


// Header worker thread
DWORD CMIDIOutDevice::HeaderProc(LPVOID Parameter)
{
    CMIDIOutDevice *Device; 
    
    Device = reinterpret_cast<CMIDIOutDevice *>(Parameter);

    // Continue while the MIDI output device is open
    while(Device->m_State == OPENED)
    {
        ::WaitForSingleObject(Device->m_Event, INFINITE);

        // Make sure the device is still open
        if(Device->m_State == OPENED)
        {
            // Remove the finished header
            Device->m_HdrQueue.RemoveHeader();
        }
    }

    return 0;
}



