//-- includes -----
#include "ClientRequestManager.h"
#include "ClientNetworkManager.h"
#include "PSMoveProtocolInterface.h"
#include "PSMoveProtocol.pb.h"
#include <cassert>
#include <map>
#include <utility>

//-- definitions -----
struct RequestContext
{
    RequestPtr request;  // std::shared_ptr<PSMoveProtocol::Request>
};
typedef std::map<int, RequestContext> t_request_context_map;
typedef std::map<int, RequestContext>::iterator t_request_context_map_iterator;
typedef std::pair<int, RequestContext> t_id_request_context_pair;
typedef std::vector<ResponsePtr> t_response_reference_cache;
typedef std::vector<RequestPtr> t_request_reference_cache;

class ClientRequestManagerImpl
{
public:
    ClientRequestManagerImpl(
        ClientPSMoveAPI::t_response_callback callback, 
        void *userdata)
        : m_callback(callback)
        , m_callback_userdata(userdata)
        , m_pending_requests()
        , m_next_request_id(0)
    {
    }

    void flush_response_cache()
    {
        // Drop all of the request/response references,
        // NOTE: std::vector::clear() calls the destructor on each element in the vector
        // This will decrement the last ref count to the parameter data, causing them to get cleaned up.
        m_request_reference_cache.clear();
        m_response_reference_cache.clear();
    }

    void send_request(RequestPtr request)
    {
        RequestContext context;

        request->set_request_id(m_next_request_id);
        ++m_next_request_id;

        context.request= request;

        // Add the request to the pending request map.
        // Requests should never be double registered.
        assert(m_pending_requests.find(request->request_id()) == m_pending_requests.end());
        m_pending_requests.insert(t_id_request_context_pair(request->request_id(), context));

        // Send the request off to the network manager to get sent to the server
        ClientNetworkManager::get_instance()->send_request(request);
    }

    void handle_request_canceled(RequestPtr request)
    {
        // Create a general canceled result
        ResponsePtr response(new PSMoveProtocol::Response);

        response->set_type(PSMoveProtocol::Response_ResponseType_GENERAL_RESULT);
        response->set_request_id(request->request_id());
        response->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_CANCELED);

        handle_response(response);
    }

    void handle_response(ResponsePtr response)
    {
        // Get the request awaiting completion
        t_request_context_map_iterator pending_request_entry= m_pending_requests.find(response->request_id());
        assert(pending_request_entry != m_pending_requests.end());

        // The context holds everything a handler needs to evaluate a response
        const RequestContext &context= pending_request_entry->second;

        // Notify the callback of the response
        if (m_callback != nullptr)
        {
            ClientPSMoveAPI::ResponseMessage response_message;

            // Generate a client API response message from 
            build_response_message(context.request, response, &response_message);

            m_callback(&response_message, m_callback_userdata);
        }

        // Remove the pending request from the map
        m_pending_requests.erase(pending_request_entry);
    }

    void build_response_message(
        RequestPtr request,
        ResponsePtr response,
        ClientPSMoveAPI::ResponseMessage *out_response_message)
    {        
        memset(out_response_message, 0, sizeof(ClientPSMoveAPI::ResponseMessage));

        // Let the response know what request the result came from
        out_response_message->request_id = request->request_id();

        // Translate internal result codes into public facing result codes
        switch (response->result_code())
        {
        case PSMoveProtocol::Response_ResultCode_RESULT_OK:
            out_response_message->result_code = ClientPSMoveAPI::_clientPSMoveResultCode_ok;
            break;
        case PSMoveProtocol::Response_ResultCode_RESULT_ERROR:
            out_response_message->result_code = ClientPSMoveAPI::_clientPSMoveResultCode_error;
            break;
        case PSMoveProtocol::Response_ResultCode_RESULT_CANCELED:
            out_response_message->result_code = ClientPSMoveAPI::_clientPSMoveResultCode_canceled;
            break;
        default:
            assert(false && "Unknown response result code");
        }

        // Attach an opaque pointer to the PSMoveProtocol request.
        // Client code that has linked against PSMoveProtocol library
        // can access this pointer via the GET_PSMOVEPROTOCOL_REQUEST() macro.
        out_response_message->opaque_response_handle = static_cast<const void*>(request.get());

        // The opaque request pointer will only remain valid until the next call to update()
        // at which time the request reference cache gets cleared out.
        m_request_reference_cache.push_back(request);

        // Attach an opaque pointer to the PSMoveProtocol response.
        // Client code that has linked against PSMoveProtocol library
        // can access this pointer via the GET_PSMOVEPROTOCOL_RESPONSE() macro.
        out_response_message->opaque_response_handle = static_cast<const void*>(response.get());

        // The opaque response pointer will only remain valid until the next call to update()
        // at which time the response reference cache gets cleared out.
        m_response_reference_cache.push_back(response);

        // Write response specific data
        if (response->result_code() == PSMoveProtocol::Response_ResultCode_RESULT_OK)
        {
            switch (response->type())
            {
            case PSMoveProtocol::Response_ResponseType_CONTROLLER_LIST:
                build_controller_list_response_message(response, &out_response_message->payload.controller_list);
                out_response_message->payload_type = ClientPSMoveAPI::_responsePayloadType_ControllerList;
                break;
            case PSMoveProtocol::Response_ResponseType_TRACKER_LIST:
                build_tracker_list_response_message(response, &out_response_message->payload.tracker_list);
                out_response_message->payload_type = ClientPSMoveAPI::_responsePayloadType_TrackerList;
                break;
            default:
                out_response_message->payload_type = ClientPSMoveAPI::_responsePayloadType_Empty;
                break;
            }
        }
    }

    void build_controller_list_response_message(
        ResponsePtr response,
        ClientPSMoveAPI::ResponsePayload_ControllerList *controller_list)
    {
        int controller_count = 0;

        // Copy the controller entries into the response payload
        while (controller_count < response->result_controller_list().controllers_size()
                && controller_count < PSMOVESERVICE_MAX_CONTROLLER_COUNT)
        {
            const auto &ControllerResponse = response->result_controller_list().controllers(controller_count);

            // Convert the PSMoveProtocol controller enum to the public ClientControllerView enum
            ClientControllerView::eControllerType controllerType;
            switch (ControllerResponse.controller_type())
            {
            case PSMoveProtocol::PSMOVE:
                controllerType = ClientControllerView::PSMove;
                break;
            case PSMoveProtocol::PSNAVI:
                controllerType = ClientControllerView::PSNavi;
                break;
            default:
                assert(0 && "unreachable");
                controllerType = ClientControllerView::PSMove;
            }

            // Add an entry to the controller list
            controller_list->controller_type[controller_count] = controllerType;
            controller_list->controller_id[controller_count] = ControllerResponse.controller_id();
            ++controller_count;
        }

        // Record how many controllers we copied into the payload
        controller_list->count = controller_count;
    }

    void build_tracker_list_response_message(
        ResponsePtr response,
        ClientPSMoveAPI::ResponsePayload_TrackerList *tracker_list)
    {
        int tracker_count = 0;

        // Copy the controller entries into the response payload
        while (tracker_count < response->result_tracker_list().trackers_size()
            && tracker_count < PSMOVESERVICE_MAX_TRACKER_COUNT)
        {
            const auto &TrackerResponse = response->result_tracker_list().trackers(tracker_count);
            ClientTrackerInfo &TrackerInfo= tracker_list->trackers[tracker_count];

            TrackerInfo.tracker_id = TrackerResponse.tracker_id();

            switch (TrackerResponse.tracker_type())
            {
            case PSMoveProtocol::TrackerType::PS3EYE:
                TrackerInfo.tracker_type = eTrackerType::PS3Eye;
                break;
            default:
                assert(0 && "unreachable");
            }

            switch (TrackerResponse.tracker_driver())
            {
            case PSMoveProtocol::TrackerDriver::LIBUSB:
                TrackerInfo.tracker_driver = eTrackerDriver::LIBUSB;
                break;
            case PSMoveProtocol::TrackerDriver::CL_EYE:
                TrackerInfo.tracker_driver = eTrackerDriver::CL_EYE;
                break;
            case PSMoveProtocol::TrackerDriver::CL_EYE_MULTICAM:
                TrackerInfo.tracker_driver = eTrackerDriver::CL_EYE_MULTICAM;
                break;
            case PSMoveProtocol::TrackerDriver::GENERIC_WEBCAM:
                TrackerInfo.tracker_driver = eTrackerDriver::GENERIC_WEBCAM;
                break;
            default:
                assert(0 && "unreachable");
            }

            TrackerInfo.tracker_focal_lengths = 
                PSMoveFloatVector2::create(
                    TrackerResponse.tracker_focal_lengths().x(), 
                    TrackerResponse.tracker_focal_lengths().y());
            TrackerInfo.tracker_principal_point =
                PSMoveFloatVector2::create(
                    TrackerResponse.tracker_principal_point().x(),
                    TrackerResponse.tracker_principal_point().y());
            TrackerInfo.tracker_screen_dimensions =
                PSMoveFloatVector2::create(
                    TrackerResponse.tracker_screen_dimensions().x(),
                    TrackerResponse.tracker_screen_dimensions().y());

            strncpy(TrackerInfo.device_path, TrackerResponse.device_path().c_str(), sizeof(TrackerInfo.device_path));
            strncpy(TrackerInfo.shared_memory_name, TrackerResponse.shared_memory_name().c_str(), sizeof(TrackerInfo.shared_memory_name));

            ++tracker_count;
        }

        // Record how many trackers we copied into the payload
        tracker_list->count = tracker_count;
    }

private:
    ClientPSMoveAPI::t_response_callback m_callback;
    void *m_callback_userdata;
    t_request_context_map m_pending_requests;
    int m_next_request_id;

    // These vectors is used solely to keep the ref counted pointers to the 
    // request/response parameter data valid until the next update call.
    // The ClientAPI message queue contains raw void pointers to the request/response and event data.
    t_request_reference_cache m_request_reference_cache;
    t_response_reference_cache m_response_reference_cache;
};

//-- public methods -----
ClientRequestManager::ClientRequestManager(ClientPSMoveAPI::t_response_callback callback, void *userdata)
{
    m_implementation_ptr = new ClientRequestManagerImpl(callback, userdata);
}

ClientRequestManager::~ClientRequestManager()
{
    delete m_implementation_ptr;
}

void ClientRequestManager::flush_response_cache()
{
    m_implementation_ptr->flush_response_cache();
}

void ClientRequestManager::send_request(
    RequestPtr request)
{
    m_implementation_ptr->send_request(request);
}

void ClientRequestManager::handle_request_canceled(RequestPtr request)
{
    m_implementation_ptr->handle_request_canceled(request);
}

void ClientRequestManager::handle_response(ResponsePtr response)
{
    m_implementation_ptr->handle_response(response);
}