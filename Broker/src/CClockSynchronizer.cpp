////////////////////////////////////////////////////////////////////////////////
/// @file         CClockSynchronizer.cpp
///
/// @author       Stephen Jackson <scj7t4@mst.edu>
///
/// @project      FREEDM DGI
///
/// @description  Handles the exchanges and mathematics to synchronize clocks
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

#include "CBroker.hpp"
#include "CClockSynchronizer.hpp"
#include "CConnectionManager.hpp"
#include "CGlobalConfiguration.hpp"
#include "CGlobalPeerList.hpp"
#include "CLogger.hpp"
#include "IPeerNode.hpp"
#include "Messages.hpp"
#include "messages/ModuleMessage.pb.h"

#include <memory>
#include <utility>

#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/foreach.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/smart_ptr.hpp>

namespace freedm {
    namespace broker {

namespace {

/// This file's logger.
CLocalLogger Logger(__FILE__);
const int MAX_REGRESSION_ENTRIES = 200;
const double SYNCHRONIZER_LAMBDA = .99999;
const int QUERY_INTERVAL = 10000;

}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::CClockSynchronizer
/// @description Constructs the synchronizer object
/// @limitations none
/// @pre None
/// @post Read handlers are registered and the synchronizer is ready to begin
/// @param ios the broker's ioservice
///////////////////////////////////////////////////////////////////////////////
CClockSynchronizer::CClockSynchronizer(boost::asio::io_service& ios)
    : m_exchangetimer(ios),
      m_uuid(CConnectionManager::Instance().GetUUID())
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    MapIndex ii(m_uuid,m_uuid);
    m_offsets[ii] = boost::posix_time::milliseconds(0);
    SetWeight(ii, 1.0);
    m_skews[ii] = 0.0;
    m_lastinteraction = boost::posix_time::microsec_clock::universal_time();
    m_kcounter = 0;
    m_myoffset = boost::posix_time::milliseconds(0);
    m_myskew = 0.0;
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::Run()
/// @description Starts the timer that handles most of the processing
/// @limitations none
/// @pre None
/// @post The timer is set and the first round will run at QUERY_INTERVAL
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::Run()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    m_exchangetimer.expires_from_now(boost::posix_time::milliseconds(QUERY_INTERVAL));
    m_exchangetimer.async_wait(boost::bind(&CClockSynchronizer::Exchange,this,
        boost::asio::placeholders::error));
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::Stop()
/// @description Stops the timer that handles most of the processing
/// @limitations none
/// @pre None
/// @post The timer is canceled.
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::Stop()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    m_exchangetimer.cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// "Downcasts" incoming messages into a specific message type, and passes the
/// message to an appropriate handler.
///
/// @param msg the incoming message
/// @param peer the node that sent this message (could be this DGI)
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::HandleIncomingMessage(
    boost::shared_ptr<const ModuleMessage> msg, PeerNodePtr peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;

    if (msg->type() != ModuleMessage::CLOCK_SYNCHRONIZER_MESSAGE)
    {
        Logger.Warn << "Dropped message of unexpected type:\n" << msg->DebugString();
        return;
    }
    ClockSynchronizerMessage csm = msg->clock_synchronizer_message();
    switch (csm.type())
    {
    case ClockSynchronizerMessage::EXCHANGE_MESSAGE:
        HandleExchange(csm.exchange_message(), peer);
        break;
    case ClockSynchronizerMessage::EXCHANGE_RESPONSE_MESSAGE:
        HandleExchangeResponse(csm.exchange_response_message(), peer);
        break;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::HandleExchange
/// @description Responds to a challenge issued by a remote node.
/// @limitations none
/// @pre None
/// @post No change
/// @param msg The message from the remote node
/// @param peer The peer sending the message.
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::HandleExchange(const ExchangeMessage& msg, PeerNodePtr peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;

    // Respond to the query ID
    peer->Send(CreateExchangeResponse(msg.query()));
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::HandleExchangeResponse
/// @description Handles the challenge response from the remote node
/// @limitations none
/// @pre None
/// @post Internal tables and state are updated, based on a linear regression
///     created from this response and previous responses.
/// @param msg The message from the remote node
/// @param peer The remote node.
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::HandleExchangeResponse(const ExchangeResponseMessage& msg, PeerNodePtr peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    std::string sender = peer->GetUUID();
    MapIndex ij(m_uuid,sender);
    boost::posix_time::ptime challenge;
    boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
    boost::posix_time::ptime response =
        boost::posix_time::time_from_string(msg.unsynchronized_sendtime());
    unsigned int k = msg.response();
    Logger.Debug<<__FILE__<<":"<<__LINE__<<std::endl;
    if(m_queries.find(ij) == m_queries.end() || m_queries[ij].first != k)
        return;
    challenge = m_queries[ij].second;
    m_queries.erase(ij);
    ResponseMap::iterator rit = m_responses.find(ij);
    ResponseList rlist;
    if(rit == m_responses.end())
    {
        m_responses[ij] = rlist;
    }
    else
    {
        rlist = rit->second;
    }
    // Add the newest response to the response list.
    rlist.push_back(TimeTuple(response,challenge));
    rlist.push_back(TimeTuple(response,now));
    if(rlist.size() > MAX_REGRESSION_ENTRIES*2)
    {
        rlist.pop_front();
        rlist.pop_front();
    }
    m_responses[ij] = rlist;
    // Now we can compute a linear regression on the contents
    // First, compute the average time
    // Pick a time to use as the base.
    boost::posix_time::ptime base = now;
    /* A note -
    The original paper had you calculate a skew and apply it to your clock,
    which is bananas because you can't change the rate that a clock ticks,
    you can compute a skewed clock reading, but it is going to be super
    messy because it will probably mean you are multiplying a double against
    all time that has ever occured (it will blow up, I think unless the
    value is super close to 1). So, I've come up with a clever trick! We
    are going to set the points we are computing a linear regression on
    to be in the past and we are going to determine the x intercept starting
    from now, which should be pretty close to what we want as an offset w/o
    having to apply any skew. */
    boost::posix_time::time_duration sumx;
    boost::posix_time::time_duration sumy;
    boost::posix_time::time_duration sumlag;
    bool even = false;
    BOOST_FOREACH(TimeTuple t, rlist)
    {
        sumx += t.first-base;
        sumy += t.second-base;
        if(even == false)
        {
            sumlag -= t.second-base;
            even = true;
        }
        else
        {
            sumlag += t.second-base;
            even = false;
        }
    }
    double lag = (TDToDouble(sumlag))/rlist.size();
    if(lag < 0.015)
    {
        Logger.Notice<<"Computed lag ("<<sender<<"): "<<lag<<std::endl;
    }
    else
    {
        Logger.Warn<<"Computed lag ("<<sender<<"): "<<lag<<std::endl;
    }
    double dxbar = TDToDouble(sumx)/rlist.size();
    double dybar = TDToDouble(sumy)/rlist.size();
    boost::posix_time::time_duration xbar = DoubleToTD(dxbar);
    boost::posix_time::time_duration ybar = DoubleToTD(dybar);
    // Now that the averages are computed we can compute Cij and fij
    double fij;
    double tmp1 = 0.0;
    double tmp2 = 0.0;
    double tmp3 = 0.0;
    double tmp4 = 0.0;
    BOOST_FOREACH(TimeTuple t, rlist)
    {
        tmp1 = TDToDouble((t.first-base)-xbar);
        tmp2 = TDToDouble((t.second-base)-ybar);
        tmp3 += tmp1 * tmp2;
        tmp4 += tmp1 * tmp1;
    }
    //If there is no spread, then we only have one xcoordinate to use.
    if(tmp4 != 0.0)
        fij = (tmp3/tmp4);
    else
        fij = 1.0;
    // And this part is Cij
    double alpha = (dybar-fij*dxbar);
    if(alpha <= 0)
        alpha += lag;
    else
        alpha -= lag;
    // Wowza!
    m_offsets[ij] = -DoubleToTD(alpha);
    SetWeight(ij, 1);
    m_skews[ij] = fij-1;
    for (unsigned i = 0; i < static_cast<unsigned>(msg.table_entry_size()); ++i)
    {
        const ExchangeResponseMessage::TableEntry te = msg.table_entry(i);
        std::string neighbor = te.uuid();
        if(neighbor == peer->GetUUID() || neighbor == m_uuid)
            continue;
        boost::posix_time::time_duration cjl = boost::posix_time::seconds(te.offset_secs())+boost::posix_time::microseconds(te.offset_fracs());
        double wjl = te.weight()-.1; // Abritrarily remove some trust to account for lag.
        double fjl = te.skew();
        MapIndex il(m_uuid,neighbor);
        if(m_offsets.find(il) == m_offsets.end())
        {
            m_offsets[il] = boost::posix_time::milliseconds(0);
            SetWeight(il, 0.0);
            m_skews[il] = 0.0;
        }
        if(GetWeight(il) < wjl)
        {
            m_offsets[il] = m_offsets[ij] + cjl;
            SetWeight(il, wjl);
            m_skews[il] = m_skews[ij] + fjl;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::Exchange
/// @description Issues a series of challenges to the remote nodes.
/// @limitations none
/// @pre None
/// @post The query table is updated and the clock offset / skew is adjusted
/// @param err The reason this function was called.
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::Exchange(const boost::system::error_code& err)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    if(err)
        return;
    // Loop through the peers and send them beacons
    std::deque< boost::shared_ptr<IPeerNode> > tmplist;
    std::deque< boost::shared_ptr<IPeerNode> > tmplist2;
    bool flop = false;
    BOOST_FOREACH(boost::shared_ptr<IPeerNode> peer, CGlobalPeerList::instance().PeerList() | boost::adaptors::map_values)
    {
        if(peer->GetUUID() == m_uuid)
           flop = true;
        else if(flop == false)
            tmplist2.push_back(peer); // put elements before me into list b
        else
            tmplist.push_back(peer); // put elements after me into list a
    }
    // put elements from list b into list a
    tmplist.insert(tmplist.end(),tmplist2.begin(),tmplist2.end());
    // This should do a circular shift of the queries, which SHOULD help with traffic if I have postulated correctly.
    BOOST_FOREACH(boost::shared_ptr<IPeerNode> peer, tmplist)
    {
        peer->Send(CreateExchangeMessage(m_kcounter));
        MapIndex ij(m_uuid,peer->GetUUID());
        m_queries[ij] = QueryRecord(m_kcounter, boost::posix_time::microsec_clock::universal_time());
    }
    m_kcounter++;
    // Run this every so often
    m_exchangetimer.expires_from_now(boost::posix_time::milliseconds(QUERY_INTERVAL));
    m_exchangetimer.async_wait(boost::bind(&CClockSynchronizer::Exchange,this,
        boost::asio::placeholders::error));
    //make sure the self referential entries stay sane.
    MapIndex ii(m_uuid,m_uuid);
    m_offsets[ii] = boost::posix_time::milliseconds(0);
    SetWeight(ii, 1.0);
    m_skews[ii] = 0.0;
    //First, we compute our personal offset and skew:
    double tmp1 = 0.0;
    double tmp2 = 0.0;
    double tmp3 = 0.0;
    OffsetMap::iterator oit;
    for(oit = m_offsets.begin(); oit != m_offsets.end(); oit++)
    {
        tmp1 += GetWeight(oit->first) * TDToDouble(oit->second);
        tmp2 += GetWeight(oit->first);
        tmp3 += GetWeight(oit->first) * m_skews[oit->first];
    }
    if(tmp2 != 0.0)
    {
        tmp1 /= tmp2;
        tmp3 /= tmp2;
        m_myoffset = DoubleToTD(tmp1);
        Logger.Notice<<"Adjusting Skew to "<<m_myoffset<<std::endl;
        CGlobalConfiguration::Instance().SetClockSkew(m_myoffset);
        m_myskew = tmp3;
    }
    //Now we adjust the CRAP out of our offset and skew table.
    /*
    for(oit = m_offsets.begin(); oit != m_offsets.end(); oit++)
    {
        oit->second -= DoubleToTD(tmp1);
        m_skews[oit->first] -= tmp3;
    }
    */
    m_offsets[ii] = boost::posix_time::milliseconds(0);
    SetWeight(ii, 1.0);
    m_skews[ii] = 0.0;
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::ExchangeMessage
/// @description Generates the challenge message
/// @limitations none
/// @pre None
/// @post None
/// @param k The sequence number to be delivered so that old messages are not
///     used.
///////////////////////////////////////////////////////////////////////////////
ModuleMessage CClockSynchronizer::CreateExchangeMessage(unsigned int k)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    ClockSynchronizerMessage csm;
    csm.set_type(ClockSynchronizerMessage::EXCHANGE_MESSAGE);
    ExchangeMessage* em = csm.mutable_exchange_message();
    em->set_query(k);
    return PrepareForSending(csm);
}


///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::ExchangeResponse
/// @description Generates the response message
/// @limitations none
/// @pre None
/// @post None
/// @param k The sequence number to be delivered so that old messages are not
///     used.
///////////////////////////////////////////////////////////////////////////////
ModuleMessage CClockSynchronizer::CreateExchangeResponse(unsigned int k)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    ClockSynchronizerMessage csm;
    csm.set_type(ClockSynchronizerMessage::EXCHANGE_RESPONSE_MESSAGE);
    ExchangeResponseMessage* erm = csm.mutable_exchange_response_message();
    erm->set_response(k);
    erm->set_unsynchronized_sendtime(boost::posix_time::to_simple_string(
        boost::posix_time::microsec_clock::universal_time()));
    for(OffsetMap::iterator oit=m_offsets.begin(); oit != m_offsets.end(); oit++)
    {
        ExchangeResponseMessage::TableEntry* te = erm->add_table_entry();
        te->set_uuid(oit->first.second);
        te->set_offset_secs(oit->second.total_seconds());
        te->set_offset_fracs(oit->second.fractional_seconds());
        te->set_skew(m_skews[oit->first]);
        te->set_weight(GetWeight(oit->first));
    }
    return PrepareForSending(csm);
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::GetSynchronizedTime
/// @description Returns the time adjusted by the set offset
/// @limitations none
/// @pre None
/// @post None
/// @return The adjusted time.
///////////////////////////////////////////////////////////////////////////////
boost::posix_time::ptime CClockSynchronizer::GetSynchronizedTime() const
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
    return now + CGlobalConfiguration::Instance().GetClockSkew();
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::GetWeight
/// @description Returns the confidence in the relative offset between 2 nodes
/// @limitations none
/// @pre None
/// @post None
/// @return The weight
///////////////////////////////////////////////////////////////////////////////
double CClockSynchronizer::GetWeight(MapIndex i) const
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    WeightMap::const_iterator it = m_weights.find(i);
    boost::posix_time::ptime set;
    if(i == MapIndex(m_uuid,m_uuid))
        return 1.0;
    if(it == m_weights.end())
        throw std::runtime_error("Can't find that index in the weights table");
    return it->second.first * pow(SYNCHRONIZER_LAMBDA,m_kcounter-m_lastresponse.find(i)->second);
}

///////////////////////////////////////////////////////////////////////////////
/// CClockSynchronizer::setWeight
/// @description Updates the weights table
/// @limitations none
/// @pre None
/// @post None
/// @param i The weight to update
/// @param w The amount of weight
///////////////////////////////////////////////////////////////////////////////
void CClockSynchronizer::SetWeight(MapIndex i, double w)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    DecayingWeight weight(w, boost::posix_time::microsec_clock::universal_time());
    m_weights[i] = weight;
    m_lastresponse[i] = m_kcounter;
}

///////////////////////////////////////////////////////////////////////////////
/// @fn CClockSynchronizer::TDToDouble
/// @description give a time duration td, convert it to a double which represents
///     the number of seconds the time duration represents
/// @param td the time duration to convert
/// @return a double of the time duration, in seconds.
///////////////////////////////////////////////////////////////////////////////
double CClockSynchronizer::TDToDouble(boost::posix_time::time_duration td)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    double x = td.total_seconds() + (td.fractional_seconds()*1.0)/1000000;
    return x;
}

///////////////////////////////////////////////////////////////////////////////
/// @fn CClockSynchronizer::DoubleToTD
/// @description given a double td, convert it to a boost time_duration of
///     roughly the same value (some losses for the accuracy of posix_time
/// @param td the time duration (in seconds) to convert
/// @return a time duration that is roughly the same as td.
///////////////////////////////////////////////////////////////////////////////
boost::posix_time::time_duration CClockSynchronizer::DoubleToTD(double td)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    double seconds, tmp, fractional;
    tmp = modf(td, &seconds);
    tmp *= 1000000; // Shift out to the microseconds
    modf(tmp, &fractional);
    return boost::posix_time::seconds(seconds) + boost::posix_time::microseconds(fractional);
}

///////////////////////////////////////////////////////////////////////////////
/// Wraps a ClockSynchronizerMessage in a ModuleMessage.
///
/// @param message the message to prepare. If any required field is unset,
///     the DGI will abort.
///
/// @return a ModuleMessage containing a copy of the ClockSynchronizerMessage
///////////////////////////////////////////////////////////////////////////////
ModuleMessage CClockSynchronizer::PrepareForSending(const ClockSynchronizerMessage& message)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    return broker::PrepareForSending(message, ModuleMessage::CLOCK_SYNCHRONIZER_MESSAGE, "clk");
}

}
}
