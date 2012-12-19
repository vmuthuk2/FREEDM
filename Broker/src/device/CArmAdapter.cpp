////////////////////////////////////////////////////////////////////////////////
/// @file           CArmAdapter.cpp
///
/// @author         Thomas Roth <tprfh7@mst.edu>
///
/// @project        FREEDM DGI
///
/// @description    Adapter for plug-and-play devices on an ARM board.
///
/// @functions
///     CArmAdapter::Create
///     CArmAdapter::CArmAdapter
///     CArmAdapter::~CArmAdapter
///     CArmAdapter::Start
///     CArmAdapter::Heartbeat
///     CArmAdapter::GetPortNumber
///     CArmAdapter::Timeout
///     CArmAdapter::HandleMessage
///     CArmAdapter::ReadStatePacket
///     CArmAdapter::SendCommandPacket
///
/// These source code files were created at Missouri University of Science and
/// Technology, and are intended for use in teaching or research. They may be
/// freely copied, modified, and redistributed as long as modified versions are
/// clearly marked as such and this notice is not removed. Neither the authors
/// nor Missouri S&T make any warranty, express or implied, nor assume any legal
/// responsibility for the accuracy, completeness, or usefulness of these files
/// or any information distributed with these files.
///
/// Suggested modifications or questions about these files can be directed to
/// Dr. Bruce McMillin, Department of Computer Science, Missouri University of
/// Science and Technology, Rolla, MO 65409 <ff@mst.edu>.
////////////////////////////////////////////////////////////////////////////////

#include "CArmAdapter.hpp"
#include "CAdapterFactory.hpp"
#include "CLogger.hpp"

#include <map>
#include <sstream>
#include <stdexcept>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>
#include <boost/property_tree/ptree.hpp>

namespace freedm {
namespace broker {
namespace device {

namespace {
/// This file's logger.
CLocalLogger Logger(__FILE__);
}

////////////////////////////////////////////////////////////////////////////////
/// Creates a new shared instance of the ARM adapter.
///
/// @pre None.
/// @post Constructs a new ARM adapter.
/// @param service The i/o service for the internal TCP server.
/// @param p The property tree that specifies the adapter configuration.
/// @return shared_ptr to the new adapter.
///
/// @limitations None.
////////////////////////////////////////////////////////////////////////////////
IAdapter::Pointer CArmAdapter::Create(boost::asio::io_service & service,
        boost::property_tree::ptree & p)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    return CArmAdapter::Pointer(new CArmAdapter(service, p));
}

////////////////////////////////////////////////////////////////////////////////
/// Constructs a new ARM adapter.
///
/// @pre The ptree must have the 'identifier' and 'stateport' properties.
/// @post Creates a new TCP server on the specified 'stateport'.
/// @post Registers CArmAdapter::HandleMessage with m_server.
/// @param service The i/o service for the TCP server.
/// @param p The property tree that configures the adapter.
///
/// @limitations None.
////////////////////////////////////////////////////////////////////////////////
CArmAdapter::CArmAdapter(boost::asio::io_service & service,
        boost::property_tree::ptree & p)
    : m_heartbeat(service)
    , m_command(service)
    , m_initialized(0)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    m_identifier = p.get<std::string>("identifier");
    m_port = p.get<unsigned short>("stateport");
    
    IServer::ConnectionHandler handler;
    handler = boost::bind(&CArmAdapter::HandleMessage, this, _1);
    m_server = CTcpServer::Create(service, m_port);
    m_server->RegisterHandler(handler);
}

////////////////////////////////////////////////////////////////////////////////
/// Destructor for the ARM adapter.
///
/// @pre None.
/// @post Destructs this object.
///
/// @limitations None.
////////////////////////////////////////////////////////////////////////////////
CArmAdapter::~CArmAdapter()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
}

////////////////////////////////////////////////////////////////////////////////
/// Starts the internal countdown timer to destroy this object.
///
/// @pre None.
/// @post m_heartbeat set to call CArmAdapter::Timeout on expiration.
///
/// @limitations None.
////////////////////////////////////////////////////////////////////////////////
void CArmAdapter::Start()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    // TODO: this fella should be configurable
    m_heartbeat.expires_from_now(boost::posix_time::seconds(5));
    m_heartbeat.async_wait(boost::bind(&CArmAdapter::Timeout, this, _1));
}

////////////////////////////////////////////////////////////////////////////////
/// Refreshes the heartbeat countdown timer.
///
/// @pre m_heartbeat must not have expired.
/// @post Resets the countdown of the m_heartbeat timer.
///
/// @limitations This call will do nothing if the timer has already expired.
////////////////////////////////////////////////////////////////////////////////
void CArmAdapter::Heartbeat()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    // TODO: hello configurable option
    if( m_heartbeat.expires_from_now(boost::posix_time::seconds(5)) != 0 )
    {
        Logger.Info << "Reset an adapter heartbeat timer." << std::endl;
        m_heartbeat.async_wait(boost::bind(&CArmAdapter::Timeout, this, _1));
    }
    else
    {
        Logger.Warn << "The heartbeat timer has already expired." << std::endl;
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Accessor function for the TCP server port number.
///
/// @pre None.
/// @post Returns the value of m_port.
/// @return The port number associated with m_server.
///
/// @limitations None.
////////////////////////////////////////////////////////////////////////////////
unsigned short CArmAdapter::GetStatePort() const
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    return m_port;
}

////////////////////////////////////////////////////////////////////////////////
/// Attempts to destroy the adapter due to timeout.
///
/// @pre None.
/// @post Calls CAdapterFactory::RemoveAdapter if the timer was not reset.
/// @param e The error code that signals if the timeout has been canceled.
///
/// @limitations This function will not work as intended if the shared pointer
/// to the calling object has a reference outside of CAdapterFactory.
////////////////////////////////////////////////////////////////////////////////
void CArmAdapter::Timeout(const boost::system::error_code & e)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;

    if( !e )
    {
        Logger.Status << "Removing an adapter due to timeout." << std::endl;
        CAdapterFactory::Instance().RemoveAdapter(m_identifier);
    }
}

////////////////////////////////////////////////////////////////////////////////
///
////////////////////////////////////////////////////////////////////////////////
void CArmAdapter::HandleMessage(IServer::Pointer connection)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    std::stringstream packet, response;
    std::string header;
    
    try
    {
        packet << connection->ReceiveData();
        Heartbeat();
        
        packet >> header;
        
        Logger.Debug << "Received " << header << " packet." << std::endl;
        
        if( header == "DeviceStates" )
        {
            response << ReadStatePacket(packet.str());
        }
        else if( header == "PoliteDisconnect" )
        {
            response << "PoliteDisconnect: Accepted\r\n\r\n";
        }
        else
        {
            response << "UnknownHeader\r\n\r\n";
        }
        
        connection->SendData(response.str());
        Heartbeat();
    }
    catch(std::exception & e)
    {
        Logger.Info << m_identifier << " communication failed." << std::endl;
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////
///
////////////////////////////////////////////////////////////////////////////////
std::string CArmAdapter::ReadStatePacket(const std::string packet) const
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    std::map<std::size_t, SignalValue> temp;
    std::map<std::size_t, SignalValue>::iterator it, end;
    
    std::stringstream out;
    std::string name, signal, strval;
    
    std::size_t index;
    DeviceSignal value;
    
    out << packet;
    
    while( out >> name >> signal >> strval )
    {
        name = m_identifier + ":" + name;
        
        Logger.Debug << "Parsing: " << name << " " << signal << << std::endl;
        
        if( m_stateInfo.count(name) == 0 )
        {
            return "UnknownDevice\r\n\r\n";
        }
        
        index = m_stateInfo[name];
        value = boost::lexical_cast<DeviceSignal>(strval);
        
        if( temp.insert(std::make_pair(index, value)).second == false )
        {
            return "DuplicateDevice\r\n\r\n";
        }
    }
    
    // critical section
    {
        boost::unique_lock<boost::shared_mutex> lock(m_rxMutex);
        
        for( it = temp.begin(), end = temp.end(); it != end; it++ )
        {
            m_rxBuffer[it->first] = it->second;
        }
    }
    
    if( !m_initialized )
    {
        m_initialized = true;
        
        // TODO: time to configure something!
        m_command.expires_from_now(boost::posix_time::seconds(2));
        m_command.async_wait(boost::bind&CArmAdapter::SendCommandPacket,
                this, _1));
    }
    
    return "Received\r\n\r\n";
}

void CArmAdapter::SendCommandPacket()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    std::map<const DeviceSignal, const std::size_t>::iterator it, end;
    std::stringstream packet;
    std::string devname, signal;
    SignalValue value;
    std::size_t index;
    
    boost::unique_lock<boost::mutex> lock(m_txMutex);
    
    end = m_commandInfo.end();
    for( it = m_commandInfo.begin(); it != end; it++ )
    {
        devname = it->first.first;
        signal = it->first.second;
        
        // remove the hostname identifier
        index = devname.find_last_of(":");
        devname = devname.substr(index+1);
        
        value = m_txBuffer[it->second];
        
        packet << devname << " " << signal << " " << value << "\r\n";
    }
    packet << "\r\n";
    
    try
    {
        connection->SendData(packet.str());
        Heartbeat();
    }
    catch(std::exception & e)
    {
        // skip
    }
    
    m_command.expires_from_now(boost::posix_time::seconds(2));
    m_command.async_wait(boost::bind(&CArmAdapter::SendCommandPacket, this, _1));
}

} // namespace device
} // namespace broker
} // namespace freedm
