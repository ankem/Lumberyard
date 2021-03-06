/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
// Original file Copyright Crytek GMBH or its affiliates, used under license.

// Description : BaseInput


#include "StdAfx.h"
#include "BaseInput.h"
#include "InputDevice.h"
#include "InputCVars.h"
#include "UnicodeFunctions.h"
#include "IHardwareMouse.h"
#include "InputNotificationBus.h"
#include <IRenderer.h>
#include <AzCore/Component/EntityId.h>
#include <AzFramework/Input/Devices/Mouse/InputDeviceMouse.h>

#ifdef AZ_PLATFORM_WINDOWS
#   undef min
#   undef max
#endif

bool compareInputListener(const IInputEventListener* pListenerA, const IInputEventListener* pListenerB)
{
    CRY_ASSERT(pListenerA != NULL && pListenerB != NULL);
    if (!pListenerA || !pListenerB)
    {
        return false;
    }

    if (pListenerA->GetPriority() > pListenerB->GetPriority())
    {
        return true;
    }

    return false;
}

CBaseInput::CBaseInput()
    : m_pExclusiveListener(0)
    , m_enableEventPosting(true)
    , m_retriggering(false)
    , m_hasFocus(false)
    , m_modifiers(0)
    , m_pCVars(new CInputCVars())
    , m_platformFlags(0)
    , m_forceFeedbackDeviceIndex(EFF_INVALID_DEVICE_INDEX)
    , m_pKinectInput(0)
    , m_pNaturalPointInput(0)
{
    GetISystem()->GetISystemEventDispatcher()->RegisterListener(this);

    g_pInputCVars = m_pCVars;

    m_holdSymbols.reserve(64);
}

CBaseInput::~CBaseInput()
{
    ShutDown();
    GetISystem()->GetISystemEventDispatcher()->RemoveListener(this);

    SAFE_DELETE(m_pCVars);
    g_pInputCVars = NULL;

    SAFE_DELETE(m_pKinectInput);
    SAFE_DELETE(m_pNaturalPointInput);
}

bool CBaseInput::Init()
{
    m_modifiers = 0;
    // always pass
    m_pKinectInput = new CKinectInputNULL();
    m_pNaturalPointInput = new TNaturalPointInput;

    m_pKinectInput->Init();

    m_pNaturalPointInput->Init();
#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
    ClearAllDeviceMappings();
    PopAllContexts();
    AZ::InputRequestBus::Handler::BusConnect();
    AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextActivated);
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)

    return true;
}

void CBaseInput::PostInit()
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        (*i)->PostInit();
    }
}

void CBaseInput::Update(bool bFocus)
{
    FUNCTION_PROFILER(GetISystem(), PROFILE_INPUT);

    m_hasFocus = bFocus;

    // Update blocking inputs
    UpdateBlockingInputs();

    PostHoldEvents();

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        if ((*i)->IsEnabled())
        {
            (*i)->Update(bFocus);
        }
    }

    // send commit event after all input processing for this frame has finished
    SInputEvent event;
    event.modifiers = m_modifiers;
    event.deviceType = eIDT_Unknown;
    event.state = eIS_Changed;
    event.value = 0;
    event.keyName = "commit";
    event.keyId = eKI_SYS_Commit;
    PostInputEvent(event);

    if (m_pKinectInput)
    {
        m_pKinectInput->Update();
    }

    if (m_pNaturalPointInput)
    {
        m_pNaturalPointInput->Update();
    }
}

void CBaseInput::ShutDown()
{
#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
    AZ::InputRequestBus::Handler::BusDisconnect();
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)
    std::for_each(m_inputDevices.begin(), m_inputDevices.end(), stl::container_object_deleter());
    m_inputDevices.clear();
    m_inputDeviceNames.clear();
    m_deviceToInputsMap.clear();
}

void CBaseInput::SetExclusiveMode(EInputDeviceType deviceType, bool exclusive, void* pUser)
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: SetExclusiveMode(%d, %s)", deviceType, exclusive ? "true" : "false");
    }

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        if ((*i)->GetDeviceType() == deviceType)
        {
            IInputDevice* desiredInputDevice = *i;
            desiredInputDevice->ClearKeyState();
            desiredInputDevice->SetExclusiveMode(exclusive);

            // try to flush the device ... perform a dry update
            EnableEventPosting(false);
            desiredInputDevice->Update(m_hasFocus);
            EnableEventPosting(true);
        }
    }
}

bool CBaseInput::InputState(const TKeyName& keyName, EInputState state)
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        if ((*i)->InputState(keyName, state))
        {
            return true;
        }
    }
    return false;
}

const char* CBaseInput::GetKeyName(const SInputEvent& event) const
{
    for (TInputDevices::const_iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        const IInputDevice* pInputDevice = (*i);
        CRY_ASSERT(pInputDevice != NULL);
        if (event.deviceType == eIDT_Unknown || pInputDevice->GetDeviceType() == event.deviceType)
        {
            const char* szRet = pInputDevice->GetKeyName(event);
            if (szRet)
            {
                return szRet;
            }
        }
    }

    return "";
}

const char* CBaseInput::GetKeyName(EKeyId keyId) const
{
    TInputDevices::const_iterator iter = m_inputDevices.begin();
    TInputDevices::const_iterator iterEnd = m_inputDevices.end();
    for (; iter != iterEnd; ++iter)
    {
        const IInputDevice* pDevice = *iter;
        CRY_ASSERT(pDevice != NULL);
        const char* szKeyName = pDevice->GetKeyName(keyId);
        if (szKeyName)
        {
            return szKeyName;
        }
    }

    return NULL;
}

char CBaseInput::GetInputCharAscii(const SInputEvent& event)
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        IInputDevice* pInputDevice = (*i);
        CRY_ASSERT(pInputDevice != NULL);
        if (event.deviceType == eIDT_Unknown || pInputDevice->GetDeviceType() == event.deviceType)
        {
            char inputChar = pInputDevice->GetInputCharAscii(event);
            if (inputChar)
            {
                return inputChar;
            }
        }
    }

    return '\0';
}

#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
void CBaseInput::RequestDeviceMapping(const Input::ProfileId& profilelId, const AZ::EntityId& requester)
{
    if (m_profileIdToBoundEntityIds.find(profilelId) == m_profileIdToBoundEntityIds.end())
    {
        AZ_Error("Input", m_nextValidDeviceIndex < std::numeric_limits<AZ::u8>::max(), "Attempting to request more devices than are supported");
        AZ_WarningOnce("Input", m_nextValidDeviceIndex < std::numeric_limits<AZ::u8>::digits, "You are requesting a high volume of devices, is this intended?");
        m_profileIdToBoundEntityIds[profilelId].insert(requester);
        m_deviceIndexToProfileIdCrc[m_nextValidDeviceIndex] = profilelId;
        ++m_nextValidDeviceIndex;
    }
    else
    {
        m_profileIdToBoundEntityIds[profilelId].insert(requester);
    }
}

void CBaseInput::ClearAllDeviceMappings()
{
    m_nextValidDeviceIndex = 0;
    m_deviceIndexToProfileIdCrc.clear();
    m_profileIdToBoundEntityIds.clear();
}

Input::ProfileId CBaseInput::GetProfileIdByDeviceIndex(AZ::u8 deviceIndex)
{
    if (m_deviceIndexToProfileIdCrc.find(deviceIndex) != m_deviceIndexToProfileIdCrc.end())
    {
        return m_deviceIndexToProfileIdCrc[deviceIndex];
    }
    return Input::ProfileId();
}

void CBaseInput::PushContext(const AZStd::string& context)
{
    AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextDeactivated);
    m_contexts.push_back(context);
    AZ::InputContextNotificationBus::Event(AZ::Crc32(context.c_str()), &AZ::InputContextNotifications::OnInputContextActivated);
}

void CBaseInput::PopContext()
{
    if (m_contexts.size())
    {
        AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextDeactivated);
        m_contexts.pop_back();
        AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextActivated);
    }
}

void CBaseInput::PopAllContexts()
{
    if (m_contexts.size())
    {
        AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextDeactivated);
        m_contexts.clear();
        AZ::InputContextNotificationBus::Event(AZ::Crc32(GetCurrentContext().c_str()), &AZ::InputContextNotifications::OnInputContextActivated);
    }
}

AZStd::string CBaseInput::GetCurrentContext()
{
    if (m_contexts.size())
    {
        return m_contexts.back();
    }
    return AZStd::string("");
}

AZStd::vector<AZStd::string> CBaseInput::GetContextStack()
{
    return m_contexts;
}
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)

const char* CBaseInput::GetOSKeyName(const SInputEvent& event)
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        const char* ret = (*i)->GetOSKeyName(event);
        if (ret && *ret)
        {
            return ret;
        }
    }
    return "";
}

//////////////////////////////////////////////////////////////////////////
SInputSymbol* CBaseInput::LookupSymbol(EInputDeviceType deviceType, int deviceIndex, EKeyId keyId)
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        IInputDevice* pDevice = *i;
        if (pDevice->GetDeviceType() == deviceType && pDevice->GetDeviceIndex() == deviceIndex)
        {
            return pDevice->LookupSymbol(keyId);
        }
    }

    // if no symbol found try finding it in any device
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        IInputDevice* pDevice = *i;
        SInputSymbol* pSym = pDevice->LookupSymbol(keyId);
        if (pSym)
        {
            return pSym;
        }
    }

    return NULL;
}

const SInputSymbol* CBaseInput::GetSymbolByName(const char* name) const
{
    TInputDevices::const_iterator iter = m_inputDevices.begin();
    TInputDevices::const_iterator iterEnd = m_inputDevices.end();
    for (; iter != iterEnd; ++iter)
    {
        const IInputDevice* pDevice = *iter;
        CRY_ASSERT(pDevice != NULL);
        const SInputSymbol* pSymbol = pDevice->GetSymbolByName(name);
        if (pSymbol)
        {
            return pSymbol;
        }
    }

    return NULL;
}

void CBaseInput::ClearKeyState()
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ClearKeyState");
    }
    m_modifiers = 0;
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        (*i)->ClearKeyState();
    }
    m_retriggering = false;

    m_holdSymbols.clear();
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ~ClearKeyState");
    }
}

void CBaseInput::ClearAnalogKeyState()
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ClearAnalogKeyState");
    }

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        TInputSymbols clearedSymbols;
        (*i)->ClearAnalogKeyState(clearedSymbols);

        int clearedSize = clearedSymbols.size();

        for (int j = 0; j < clearedSize; j++)
        {
            ClearHoldEvent(clearedSymbols[j]);
        }
    }

    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ~ClearAnalogKeyState");
    }
}

void CBaseInput::RetriggerKeyState()
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: RetriggerKeyState");
    }

    m_retriggering = true;
    SInputEvent event;

    // DARIO_NOTE: In this loop, m_holdSymbols size and content is changed so previous
    // code was resulting in occasional index out of bounds.
    // Note that PostInputEvent() can either push or pop elements from m_holdSymbols.
    // The original intent of the code has been lost in time... so....
    // the fix includes checking for (i < m_holdSymbols.size()) inside the loop and
    // caching m_holdSymbols[i]

    const size_t count = m_holdSymbols.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (i < m_holdSymbols.size())
        {
            SInputSymbol* pSymbol = m_holdSymbols[i];
            EInputState oldState = pSymbol->state;
            pSymbol->state = eIS_Pressed;
            pSymbol->AssignTo(event, GetModifiers());
            event.deviceIndex = pSymbol->deviceIndex;
            PostInputEvent(event);
            pSymbol->state = oldState;
        }
    }
    m_retriggering = false;
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ~RetriggerKeyState");
    }
}

void CBaseInput::AddEventListener(IInputEventListener* pListener)
{
    // Add new listener to list if not added yet.
    if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
    {
        m_listeners.push_back(pListener);
        m_listeners.sort(compareInputListener);
    }
}

void CBaseInput::RemoveEventListener(IInputEventListener* pListener)
{
    // Remove listener if it is in list.
    TInputEventListeners::iterator it = std::find(m_listeners.begin(), m_listeners.end(), pListener);
    if (it != m_listeners.end())
    {
        m_listeners.erase(it);
    }
}

void CBaseInput::AddConsoleEventListener(IInputEventListener* pListener)
{
    if (std::find(m_consoleListeners.begin(), m_consoleListeners.end(), pListener) == m_consoleListeners.end())
    {
        m_consoleListeners.push_back(pListener);
        m_consoleListeners.sort(compareInputListener);
    }
}

void CBaseInput::RemoveConsoleEventListener(IInputEventListener* pListener)
{
    TInputEventListeners::iterator it = std::find(m_consoleListeners.begin(), m_consoleListeners.end(), pListener);
    if (it != m_consoleListeners.end())
    {
        m_consoleListeners.erase(it);
    }
}

void CBaseInput::SetExclusiveListener(IInputEventListener* pListener)
{
    m_pExclusiveListener = pListener;
}

IInputEventListener* CBaseInput::GetExclusiveListener()
{
    return m_pExclusiveListener;
}

bool CBaseInput::AddInputDevice(IInputDevice* pDevice)
{
    if (pDevice)
    {
        if (pDevice->Init())
        {
            static uint8 uniqueID = 0;
            pDevice->SetUniqueId(uniqueID++);
            m_inputDevices.push_back(pDevice);
            AZStd::string commonName = pDevice->GetDeviceCommonName();
            AZ::Crc32 commonNameCrc(commonName.c_str());
            if (AZStd::find(m_inputDeviceNames.begin(), m_inputDeviceNames.end(), commonName) == m_inputDeviceNames.end())
            {
                m_inputDeviceNames.push_back(commonName);
                for (const auto& idToSymbol : pDevice->GetInputToSymbolMappings())
                {
                    m_deviceToInputsMap[commonNameCrc].push_back(idToSymbol.second->name.c_str());
                }
            }
            return true;
        }
        delete pDevice;
    }
    return false;
}

void CBaseInput::EnableEventPosting (bool bEnable)
{
    m_enableEventPosting = bEnable;
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: EnableEventPosting(%s)", bEnable ? "true" : "false");
    }
}

bool CBaseInput::IsEventPostingEnabled() const
{
    return m_enableEventPosting;
}

void CBaseInput::PostInputEvent(const SInputEvent& event, bool bForce)
{
    FUNCTION_PROFILER(GetISystem(), PROFILE_INPUT);
    //CryAutoCriticalSection postInputLock(m_postInputEventMutex);

    if (!bForce && !m_enableEventPosting)
    {
        return;
    }

    if (event.keyId == eKI_Unknown)
    {
        return;
    }

    if (g_pInputCVars->i_debug)
    {
        // log out key press and release events
        if (event.state == eIS_Pressed || event.state == eIS_Released)
        {
            gEnv->pLog->Log("InputDebug: '%s' - %s", event.keyName.c_str(), event.state == eIS_Pressed ? "pressed" : "released");
        }
        else if ((event.state == eIS_Changed) && (g_pInputCVars->i_debug == 2))
        {
            gEnv->pLog->Log("InputDebug (Changed): '%s' - %.4f", event.keyName.c_str(), event.value);
        }
    }

    if (!SendEventToListeners(event))
    {
        return;
    }
    AddEventToHoldSymbols(event);

    if (event.keyId == eKI_SYS_DisconnectDevice)
    {
        // Zero everything out
        RemoveDeviceHoldSymbols(event.deviceType, event.deviceIndex);
        ClearKeyState();
        ClearAnalogKeyState();
    }
}

void CBaseInput::PostUnicodeEvent(const SUnicodeEvent& event, bool bForce)
{
    FUNCTION_PROFILER(GetISystem(), PROFILE_INPUT);
    assert(event.inputChar != 0 && Unicode::Validate(event.inputChar) && "Attempt to post invalid unicode event");

    if (!bForce && !m_enableEventPosting)
    {
        return;
    }

    if (g_pInputCVars->i_debug)
    {
        char utf8_buf[5];
        Unicode::Convert(utf8_buf, event.inputChar);
        gEnv->pLog->Log("InputDebug: Unicode input codepoint (%u), %s", event.inputChar, utf8_buf);
    }

    if (!SendEventToListeners(event))
    {
        return;
    }
}

bool CBaseInput::SendEventToListeners(const SInputEvent& event)
{
    // console listeners get to process the event first
    for (TInputEventListeners::const_iterator it = m_consoleListeners.begin(); it != m_consoleListeners.end(); ++it)
    {
        bool ret = (*it)->OnInputEvent(event);
        if (ret)
        {
            return false;
        }
    }

    // pass the event on to Ebus listeners
#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
    if (!gEnv->pConsole->IsOpened())
    {
        Input::ProfileId profilelId = GetProfileIdByDeviceIndex(event.deviceIndex);
        if (profilelId != Input::BroadcastProfile)
        {
            AZ::InputNotificationBus::Event(AZ::InputNotificationId(profilelId, event.crc), &AZ::InputNotification::OnNotifyInputEvent, event);
            AZ::InputNotificationBus::Event(AZ::CreateAllEventsOnProfileNotificationId(profilelId), &AZ::InputNotification::OnNotifyInputEvent, event);
        }
        AZ::InputNotificationBus::Event(AZ::CreateSpecificEventNotificationId(event.crc), &AZ::InputNotification::OnNotifyInputEvent, event);
        AZ::InputNotificationBus::Event(AZ::CreateBroadcastNotificationId(), &AZ::InputNotification::OnNotifyInputEvent, event);
    }
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)

    // exclusive listener can filter out the event if he wants to and cause this call to return
    // before any of the regular listeners get to process it
    if (m_pExclusiveListener)
    {
        bool ret = m_pExclusiveListener->OnInputEvent(event);
        if (ret)
        {
            return false;
        }
    }

    // Don't post events if input is being blocked
    // but still need to update held inputs
    bool bInputBlocked = ShouldBlockInputEventPosting(event.keyId, event.deviceType, event.deviceIndex);
    if (!bInputBlocked)
    {
        // Send this event to all listeners until the first one returns true.
        for (TInputEventListeners::const_iterator it = m_listeners.begin(); it != m_listeners.end(); ++it)
        {
            assert(*it);
            if (*it == NULL)
            {
                continue;
            }

            bool ret = (*it)->OnInputEvent(event);
            if (ret)
            {
                break;
            }
        }
    }

    return true;
}

bool CBaseInput::SendEventToListeners(const SUnicodeEvent& event)
{
    // console listeners get to process the event first
    for (TInputEventListeners::const_iterator it = m_consoleListeners.begin(); it != m_consoleListeners.end(); ++it)
    {
        bool ret = (*it)->OnInputEventUI(event);
        if (ret)
        {
            return false;
        }
    }

    // exclusive listener can filter out the event if he wants to and cause this call to return
    // before any of the regular listeners get to process it
    if (m_pExclusiveListener)
    {
        bool ret = m_pExclusiveListener->OnInputEventUI(event);
        if (ret)
        {
            return false;
        }
    }

    // Send this event to all listeners until the first one returns true.
    for (TInputEventListeners::const_iterator it = m_listeners.begin(); it != m_listeners.end(); ++it)
    {
        assert(*it);
        if (*it == NULL)
        {
            continue;
        }

        bool ret = (*it)->OnInputEventUI(event);
        if (ret)
        {
            break;
        }
    }

    return true;
}

void CBaseInput::AddEventToHoldSymbols(const SInputEvent& event)
{
    if (!m_retriggering)
    {
        if (event.pSymbol && event.pSymbol->state == eIS_Pressed)
        {
            if (g_pInputCVars->i_debug)
            {
                gEnv->pLog->Log("InputDebug: 0x%p AddEventToHoldSymbols Symbol %d %s, Event %s %s", this, event.pSymbol->keyId, event.pSymbol->name.c_str(), event.keyName.c_str(), event.state == eIS_Pressed ? "pressed" : "released");
            }
            event.pSymbol->state = eIS_Down;
            //cache device index in the hold symbol
            event.pSymbol->deviceIndex = event.deviceIndex;
            m_holdSymbols.push_back(event.pSymbol);
        }
        else if (event.pSymbol && event.pSymbol->state == eIS_Released && !m_holdSymbols.empty())
        {
            ClearHoldEvent(event.pSymbol);
        }
    }
}

void CBaseInput::RemoveDeviceHoldSymbols(EInputDeviceType deviceType, uint8 deviceIndex)
{
    if (!m_holdSymbols.empty())
    {
        TInputSymbols::iterator begin = m_holdSymbols.begin();
        for (TInputSymbols::iterator it = begin; it != m_holdSymbols.end(); )
        {
            SInputSymbol* symbol = *it;
            if (symbol->deviceType == deviceType && symbol->deviceIndex == deviceIndex)
            {
                SInputEvent releaseEvent;
                releaseEvent.deviceType = deviceType;
                releaseEvent.deviceIndex = deviceIndex;
                releaseEvent.state = eIS_Released;
                releaseEvent.keyName = symbol->name;
                releaseEvent.keyId = symbol->keyId;
                releaseEvent.pSymbol = symbol;

                if (g_pInputCVars->i_debug)
                {
                    gEnv->pLog->Log("InputDebug: RemoveDeviceHoldSymbols Device %d %d, Symbol %s %d", deviceType, deviceIndex, releaseEvent.keyName.c_str(), releaseEvent.keyId);
                }

                SendEventToListeners(releaseEvent);

                it = m_holdSymbols.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

bool CBaseInput::ShouldBlockInputEventPosting(const EKeyId keyId, const EInputDeviceType deviceType, const uint8 deviceIndex) const
{
    bool bBlocked = false;
    TInputBlockData::const_iterator iter = m_inputBlockData.begin();
    TInputBlockData::const_iterator iterEnd = m_inputBlockData.end();
    for (; iter != iterEnd; ++iter)
    {
        const SInputBlockData& curBlockingInput = *iter;
        if (curBlockingInput.keyId == keyId)
        {
            // Now check if correct device index since can block certain devices only if wanted to
            if (curBlockingInput.bAllDeviceIndices) // All device indices match to this one
            {
                bBlocked = true;
                break;
            }
            else if (curBlockingInput.deviceIndex == deviceIndex) // Device index matches
            {
                bBlocked = true;
                break;
            }
        }
    }

    return bBlocked;
}

void CBaseInput::UpdateBlockingInputs()
{
    const float fElapsedTime = gEnv->pTimer->GetFrameTime(ITimer::ETIMER_UI);

    TInputBlockData::iterator iter = m_inputBlockData.begin();
    TInputBlockData::iterator iterEnd = m_inputBlockData.end();
    while (iter != iterEnd)
    {
        SInputBlockData& curBlockingInput = *iter;
        float& fBlockDuration = curBlockingInput.fBlockDuration;
        fBlockDuration -= fElapsedTime;
        if (fBlockDuration < 0.0f) // Time is up, remove now
        {
            iter = m_inputBlockData.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}

void CBaseInput::PostHoldEvents()
{
    FUNCTION_PROFILER(GetISystem(), PROFILE_INPUT);
    SInputEvent event;

    Vec2 mousePosition(ZERO);
#if defined(AZ_FRAMEWORK_INPUT_ENABLED)
    AZ::Vector2 systemCursorPositionNormalized = AZ::Vector2::CreateZero();
    AzFramework::InputSystemCursorRequestBus::EventResult(systemCursorPositionNormalized,
                                                          AzFramework::InputDeviceMouse::Id,
                                                          &AzFramework::InputSystemCursorRequests::GetSystemCursorPositionNormalized);
    mousePosition.x = systemCursorPositionNormalized.GetX() * gEnv->pRenderer->GetWidth();
    mousePosition.y = systemCursorPositionNormalized.GetY() * gEnv->pRenderer->GetHeight();
#else
    if (gEnv->pHardwareMouse)
    {
        gEnv->pHardwareMouse->GetHardwareMouseClientPosition(&(mousePosition.x), &(mousePosition.y));
    }
#endif // defined(AZ_FRAMEWORK_INPUT_ENABLED)

    // DARIO_NOTE: In this loop, m_holdSymbols size and content is changed so previous
    // code was resulting in occasional index out of bounds.
    // The desired behaviour as I understand it is as follows:
    // (PRESSED) symbols should be added to the to the end of m_holdSymbols but not
    // processed this frame (thus the keep the loop comparison as is).
    // On the other hand, (RELEASED) symbols need to be removed from the m_holdSymbols this frame
    // so the size of m_holdSymbols may decrease. The search for a RELEASED symbol is done backwards
    // i.e. from m_holdSymbols.end() - 1 to 0 thus it will never go before the sending index (i.e. i)
    // In order to allow for this behaviour, the (i < kCurrentCount) check is added.

    const size_t kInitialCount = m_holdSymbols.size();
    for (size_t i = 0; i < kInitialCount; ++i)
    {
        const size_t kCurrentCount = m_holdSymbols.size();
        if (i < kCurrentCount)
        {
            m_holdSymbols[i]->AssignTo(event, GetModifiers());
            event.deviceIndex = m_holdSymbols[i]->deviceIndex;
            event.state = eIS_Down;
            if (event.deviceType != eIDT_TouchScreen)
            {
                event.screenPosition = mousePosition;
            }

            if (g_pInputCVars->i_debug)
            {
                gEnv->pLog->Log("InputDebug: 0x%p PostHoldEvent Symbol %d %s", this, m_holdSymbols[i]->keyId, m_holdSymbols[i]->name.c_str());
            }

            PostInputEvent(event);
        }
    }
}

void CBaseInput::ClearHoldEvent(SInputSymbol* pSymbol)
{
    // remove hold key
    int slot = -1;
    int last = m_holdSymbols.size() - 1;

    for (int i = last; i >= 0; --i)
    {
        if (m_holdSymbols[i] == pSymbol)
        {
            slot = i;
            break;
        }
    }
    if (slot != -1)
    {
        if (g_pInputCVars->i_debug)
        {
            gEnv->pLog->Log("InputDebug: 0x%p ClearHoldEvent Symbol %d %s", this, pSymbol->keyId, pSymbol->name.c_str());
        }
        // swap last and found symbol
        m_holdSymbols[slot] = m_holdSymbols[last];
        // pop last ... which is now the one we want to get rid of
        m_holdSymbols.pop_back();
    }
}

void CBaseInput::ForceFeedbackEvent(const SFFOutputEvent& event)
{
    if (g_pInputCVars->i_forcefeedback == 0 || m_hasFocus == false)
    {
        return;
    }

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        if ((*i)->GetDeviceType() == event.deviceType && (m_forceFeedbackDeviceIndex == (*i)->GetDeviceIndex()))
        {
            IFFParams params;
            params.effectId = event.eventId;
            params.timeInSeconds = event.timeInSeconds;
            params.triggerData = event.triggerData;

            switch (event.eventId)
            {
            case eFF_Rumble_Basic:
                params.strengthA = event.amplifierS;
                params.strengthB = event.amplifierA;
                break;

            case eFF_Rumble_Frame:
                params.timeInSeconds = 0.0f;
                params.strengthA = event.amplifierS;
                params.strengthB = event.amplifierA;
                break;
            default:
                break;
            }

            (*i)->SetForceFeedback(params);
        }
    }
}

void CBaseInput::ForceFeedbackSetDeviceIndex(int index)
{
    if (m_forceFeedbackDeviceIndex != index)
    {
        // Disable rumble on current device before switching, otherwise it will stay on
        ForceFeedbackEvent(SFFOutputEvent(eIDT_Gamepad, eFF_Rumble_Frame, SFFTriggerOutputData::Initial::ZeroIt, 0.0f, 0.0f, 0.0f));
        m_forceFeedbackDeviceIndex = index;
    }
}

void CBaseInput::EnableDevice(EInputDeviceType deviceType, bool enable)
{
    if (deviceType < (int)m_inputDevices.size())
    {
        // try to flush the device ... perform a dry update
        EnableEventPosting(false);
        m_inputDevices[deviceType]->Update(m_hasFocus);
        EnableEventPosting(true);
        m_inputDevices[deviceType]->Enable(enable);
    }
}


bool CBaseInput::HasInputDeviceOfType(EInputDeviceType type)
{
    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        if ((*i)->IsOfDeviceType(type))
        {
            return true;
        }
    }
    return false;
}

void CBaseInput::OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam)
{
    switch (event)
    {
    case ESYSTEM_EVENT_CHANGE_FOCUS:
        ClearKeyState();
        break;
    case ESYSTEM_EVENT_LEVEL_LOAD_START:
        ClearKeyState();
#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
        ClearAllDeviceMappings();
        PopAllContexts();
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)
        break;
    case ESYSTEM_EVENT_LEVEL_GAMEPLAY_START:
        ClearKeyState();
        break;
    case ESYSTEM_EVENT_LEVEL_POST_UNLOAD:
        ClearKeyState();
        break;
    case ESYSTEM_EVENT_FAST_SHUTDOWN:
        CryLogAlways("BaseInput - Shutting down all force feedback");
        ForceFeedbackEvent(SFFOutputEvent(eIDT_Gamepad, eFF_Rumble_Basic, SFFTriggerOutputData::Initial::ZeroIt, 0.0f, 0.0f, 0.0f));
        break;
    case ESYSTEM_EVENT_LANGUAGE_CHANGE:
        for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
        {
            (*i)->OnLanguageChange();
        }
        // Clear all key states and hold symbols, as their symbol pointers have now changed
        ClearKeyState();
        break;
    case ESYSTEM_EVENT_GAME_MODE_SWITCH_START:
#if !defined(AZ_FRAMEWORK_INPUT_ENABLED)
        ClearAllDeviceMappings();
        PopAllContexts();
#endif // !defined(AZ_FRAMEWORK_INPUT_ENABLED)
        break;
    default:
        break;
    }
}

// Input blocking functionality
bool CBaseInput::SetBlockingInput(const SInputBlockData& inputBlockData)
{
    // Don't allow blocking special or unknown events
    if (inputBlockData.keyId >= eKI_SYS_Commit)
    {
        return false;
    }

    bool bSet = false;
    bool bFound = false;

    TInputBlockData::iterator iter = m_inputBlockData.begin();
    TInputBlockData::iterator iterEnd = m_inputBlockData.end();
    for (; iter != iterEnd; ++iter)
    {
        SInputBlockData& curBlockingInputData = *iter;
        if (curBlockingInputData.keyId == inputBlockData.keyId)
        {
            // Now match the deviceIndex if using it
            if ((curBlockingInputData.bAllDeviceIndices && inputBlockData.bAllDeviceIndices) ||  // All indices match
                ((!curBlockingInputData.bAllDeviceIndices && !inputBlockData.bAllDeviceIndices) &&     // Not all indices + deviceIndex matches
                 (curBlockingInputData.deviceIndex == inputBlockData.deviceIndex)))
            {
                // It already exists, but only set if the specified time exceeds current time
                if (curBlockingInputData.fBlockDuration < inputBlockData.fBlockDuration)
                {
                    curBlockingInputData.fBlockDuration = inputBlockData.fBlockDuration;
                    bSet = true;
                }
                bFound = true;
                break;
            }
        }
    }

    if (!bFound) // Not found so now add it
    {
        m_inputBlockData.push_back(inputBlockData);
        bSet = true;
    }

    return bSet;
}

bool CBaseInput::RemoveBlockingInput(const SInputBlockData& inputBlockData)
{
    bool bRemoved = false;

    TInputBlockData::iterator iter = m_inputBlockData.begin();
    TInputBlockData::iterator iterEnd = m_inputBlockData.end();
    for (; iter != iterEnd; ++iter)
    {
        const SInputBlockData& curBlockingInputData = *iter;
        // Now match the deviceIndex if using it
        if ((curBlockingInputData.bAllDeviceIndices && inputBlockData.bAllDeviceIndices) ||  // All indices match
            ((!curBlockingInputData.bAllDeviceIndices && !inputBlockData.bAllDeviceIndices) &&          // Not all indices + deviceIndex matches
             (curBlockingInputData.deviceIndex == inputBlockData.deviceIndex)))
        {
            m_inputBlockData.erase(iter);
            bRemoved = true;
            break;
        }
    }

    return bRemoved;
}

bool CBaseInput::HasBlockingInput(const SInputBlockData& inputBlockData) const
{
    bool bHasBlockingInput = false;

    TInputBlockData::const_iterator iter = m_inputBlockData.begin();
    TInputBlockData::const_iterator iterEnd = m_inputBlockData.end();
    for (; iter != iterEnd; ++iter)
    {
        const SInputBlockData& curBlockingInputData = *iter;
        // Now match the deviceIndex if using it
        if ((curBlockingInputData.bAllDeviceIndices && inputBlockData.bAllDeviceIndices) ||  // All indices match
            ((!curBlockingInputData.bAllDeviceIndices && !inputBlockData.bAllDeviceIndices) &&          // Not all indices + deviceIndex matches
             (curBlockingInputData.deviceIndex == inputBlockData.deviceIndex)))
        {
            bHasBlockingInput = true;
            break;
        }
    }

    return bHasBlockingInput;
}

int CBaseInput::GetNumBlockingInputs() const
{
    return (int)m_inputBlockData.size();
}

void CBaseInput::ClearBlockingInputs()
{
    m_inputBlockData.clear();
}

void CBaseInput::SetDeadZone(float fThreshold)
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: SetDeadZone [%0.3f]", fThreshold);
    }

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        (*i)->SetDeadZone(fThreshold);
    }

    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ~SetDeadZone");
    }
}

void CBaseInput::RestoreDefaultDeadZone()
{
    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: RestoreDefaultDeadZone");
    }

    for (TInputDevices::iterator i = m_inputDevices.begin(); i != m_inputDevices.end(); ++i)
    {
        (*i)->RestoreDefaultDeadZone();
    }

    if (g_pInputCVars->i_debug)
    {
        gEnv->pLog->Log("InputDebug: ~RestoreDefaultDeadZone");
    }
}

bool CBaseInput::GrabInput(bool bGrab)
{
    return false;
}

IInputDevice* CBaseInput::GetDevice(uint16 id, EInputDeviceType deviceType)
{
    for (TInputDevices::const_iterator it = m_inputDevices.begin(); it != m_inputDevices.end(); ++it)
    {
        IInputDevice* pDevice = *it;

        if (id == pDevice->GetDeviceIndex() && pDevice->GetDeviceType() == deviceType)
        {
            return pDevice;
        }
    }

    return NULL;
}
