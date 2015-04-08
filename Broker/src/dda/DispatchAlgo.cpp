#include "DispatchAlgo.hpp"
#include "CBroker.hpp"
#include "CLogger.hpp"
#include "Messages.hpp"
#include "CDeviceManager.hpp"
#include "CGlobalPeerList.hpp"
#include "CGlobalConfiguration.hpp"
#include "gm/GroupManagement.hpp"

#include <boost/asio/error.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace freedm{
namespace broker{
namespace dda{

const double P_max_grid = 20.0;
const double P_min_grid = 0.0;

const double P_max_desd = 5.0;
const double P_min_desd = -5.0;

const double eta = 0.5;
const double rho = 1.5; 

const int inner_iter = 5;

const double E_init[3] = {1, 1.5, 0.5};
const double E_full[3] = {5, 10, 5};

const double price_profile[3] = {5.27, 15.599, 15.599};

const int delta_time = 15;

namespace {
    /// This file's logger.
    CLocalLogger Logger(__FILE__);
}

DDAAgent::DDAAgent()
{
    m_iteration = 0;
    for(int i = 0; i<3; i++)
    { 
	m_inideltaP[i] = 0.0;
	m_inilambda[i] = 0.0;
    	m_adjdeltaP[i] = 0.0;
	m_adjlambda[i] = 0.0;
	m_deltaP1[i] = 0.0;
	m_deltaP2[i] = 0.0;    	
    }
    m_startDESDAlgo = false;
    m_timer = CBroker::Instance().AllocateTimer("dda");
}

DDAAgent::~DDAAgent()
{
}

void DDAAgent::LoadTopology()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    const std::string EDGE_TOKEN = "edge";
    const std::string VERTEX_TOKEN = "sst";
    
    //m_adjlist is the adjecent node list for each node
   
    std::string fp = CGlobalConfiguration::Instance().GetTopologyConfigPath();
    if (fp == "")
    {
        Logger.Warn << "No topology configuration file specified" << std::endl;
        return;
    }
    
    std::ifstream topf(fp.c_str());
    std::string token;

    if (!topf.is_open())
    {
        throw std::runtime_error("Couldn't open physical topology file.");
    }

    while(topf >> token)
    {
        if (token == EDGE_TOKEN)
        {   
            std::string v_symbol1;
            std::string v_symbol2;
            if (!(topf>>v_symbol1>>v_symbol2))
            {
                throw std::runtime_error("Failed to Reading Edge Topology Entry.");
            }
            Logger.Debug << "Got Edge:" << v_symbol1 << "," << v_symbol2 << std::endl;
            if (!m_adjlist.count(v_symbol1))
                m_adjlist[v_symbol1] = VertexSet();
            if (!m_adjlist.count(v_symbol2))
                m_adjlist[v_symbol2] = VertexSet();

            m_adjlist[v_symbol1].insert(v_symbol2);
            m_adjlist[v_symbol2].insert(v_symbol1);
        }
        else if (token == VERTEX_TOKEN)
        {
            std::string uuid;
            std::string vsymbol;
            if (!(topf>>vsymbol>>uuid))
            {
                throw std::runtime_error("Failed Reading Vertex Topology Entry.");
            } 
            if (uuid == GetUUID())
            {
		Logger.Debug << "The local uuid is " << GetUUID() << std::endl;
                m_localsymbol = vsymbol;
            }
            m_strans[vsymbol] = uuid;
            Logger.Debug << "Got Vertex: " << vsymbol << "->" << uuid << std::endl;
        }
        else
        {
            Logger.Error << "Unexpected token: " << token << std::endl;
            throw std::runtime_error("Physical Topology: Input Topology File is Malformed.");
        }
        token = "";
    }
    topf.close();
    
    Logger.Debug << "The local symbol is " << m_localsymbol << std::endl;
    int maxSize = 0;
    BOOST_FOREACH( const AdjacencyListMap::value_type& mp, m_adjlist)
    {
	Logger.Debug << "The vertex is " << mp.first << std::endl;
        VertexSet t = mp.second;
        // find the adjacent string set
        if (mp.first == m_localsymbol)
            m_localadj = t;

        if (t.size() >= maxSize)
        {
            maxSize = t.size();
        }
    }
    Logger.Debug << "The max connection in this topology is " << maxSize << std::endl;
    m_adjnum = m_localadj.size();
    Logger.Debug << "The local connection size is " << m_adjnum << std::endl;
    epsil = 1.0/(maxSize + 1);
    Logger.Debug << "The epsil is (in LoadTopology)" << epsil << std::endl;
}


void DDAAgent::DESDScheduledMethod(const boost::system::error_code& err)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    
    if(!err)
    {	
	Logger.Debug << "DDA has scheduled!" << std::endl;
	CBroker::Instance().Schedule(m_timer, boost::posix_time::not_a_date_time,
		boost::bind(&DDAAgent::DESDScheduledMethod, this, boost::asio::placeholders::error)); 
    }
    else
    {
	Logger.Error << err << std::endl;
    }

    LoadTopology();
    Logger.Debug << "The epsil is " << epsil << std::endl;
  
    VertexSet adjset = m_adjlist.find(m_localsymbol)->second;
    Logger.Debug << "The size of neighbors is " << adjset.size() << std::endl;

    m_adjratio = epsil;
    m_localratio = 1.0- adjset.size()*epsil;
    Logger.Debug << "The ratio for local and neighbors are " << m_localratio << " and " << m_adjratio << std::endl;

    //Figure out the attached devices in the local DGI
    int sstCount = device::CDeviceManager::Instance().GetDevicesOfType("Sst").size();
    int desdCount = device::CDeviceManager::Instance().GetDevicesOfType("Desd").size();
    int loadCount = device::CDeviceManager::Instance().GetDevicesOfType("Load").size();
    int pvCount = device::CDeviceManager::Instance().GetDevicesOfType("Pvpanel").size();
    int wtCount = device::CDeviceManager::Instance().GetDevicesOfType("Wturbine").size();

    if (sstCount == 1 || loadCount == 1 || pvCount == 1 || wtCount == 1)
    {
        if (loadCount == 1 && m_localsymbol == "3")
        {
	    m_inideltaP[0]=4.3127;
	    m_inideltaP[1]=4.2549;
            m_inideltaP[2]=4.2343;
        }
        else if (loadCount == 1 && m_localsymbol == "11")
        {
	    m_inideltaP[0]=8.8;
	    m_inideltaP[1]=8.6;
            m_inideltaP[2]=8.8;
        }
        else if (pvCount == 1 && m_localsymbol == "6")
        {
	    m_inideltaP[0]=3.8;
	    m_inideltaP[1]=2.5;
            m_inideltaP[2]=1.3;
        }
        else if (wtCount == 1 && m_localsymbol == "9")
        {
	    m_inideltaP[0]=1.8;
	    m_inideltaP[1]=1.9;
            m_inideltaP[2]=2.1;
        }
    }
    Logger.Debug << "Initialization of Load1, Load2, PV and WindTurbine have done" << std::endl;
    //sendtoAdjList();
   
}

/*
int DDAAgent::Run()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    // Scheduling for immmediate execution
    CBroker::Instance().Schedule("dda",
	boost::bind(&DDAAgent::DESDScheduledMethod, this));
    return 0;
}
*/

void DDAAgent::Run()
{
    CBroker::Instance().Schedule("dda",
	boost::bind(&DDAAgent::DESDScheduledMethod, this, boost::system::error_code()));
}

void DDAAgent::HandleIncomingMessage(boost::shared_ptr<const ModuleMessage> msg, CPeerNode peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
 
    if (msg->has_group_management_message())
    {
	gm::GroupManagementMessage gmm = msg->group_management_message();
        if (gmm.has_peer_list_message())
	{
	    HandlePeerList(gmm.peer_list_message(), peer);
	}
	else
	{
	    Logger.Warn << "Dropped unexpected group management message:\n" << msg->DebugString();
	}
    }
    else if (msg->has_desd_state_message())
    {
        HandleUpdate(msg->desd_state_message(), peer);
    }
}

void DDAAgent::HandlePeerList(const gm::PeerListMessage &m, CPeerNode peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    Logger.Debug << "Updated peer list received from: " << peer.GetUUID() << std::endl;
    if (m_startDESDAlgo == false)
    {
	m_startDESDAlgo = true;
	sendtoAdjList();
    }
}

void DDAAgent::HandleUpdate(const DesdStateMessage& msg, CPeerNode peer)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;

//  localratio = 1 - m_adjnum/epsil;
//  adjratio = 1/epsil;

    if (msg.iteration() == m_iteration )
    {
        if (m_localadj.find(msg.symbol()) != m_localadj.end())
        {
            m_adjnum--;
            if (m_adjnum != 0)
            {
                m_adjdeltaP[0] += msg.deltapstep1();
                m_adjdeltaP[1] += msg.deltapstep2();
                m_adjdeltaP[2] += msg.deltapstep3();
                m_adjlambda[0] += msg.lambdastep1();
                m_adjlambda[1] += msg.lambdastep2();
                m_adjlambda[2] += msg.lambdastep3();
            }
            else 
            {
		//DESD devices update
		if(m_localsymbol == "4" || m_localsymbol == "7" || m_localsymbol == "10")
		{
		    double aug1[3] = {0.0};
		    double aug2[3] = {0.0};    

		    aug1[0] = (m_deltaP1[0]>0?m_deltaP1[0]:0)+(m_deltaP1[1]>0?m_deltaP1[1]:0)
			      + (m_deltaP1[2]>0?m_deltaP1[2]:0);
		    aug1[1] = (m_deltaP1[1]>0?m_deltaP1[1]:0)+(m_deltaP1[2]>0?m_deltaP1[2]:0);
		    aug1[2] = m_deltaP1[2]>0?m_deltaP1[2]:0;

		    aug2[0] = (m_deltaP2[0]>0?m_deltaP2[0]:0)+(m_deltaP2[1]>0?m_deltaP2[1]:0)
			      + (m_deltaP2[2]>0?m_deltaP2[2]:0);
		    aug2[1] = (m_deltaP2[1]>0?m_deltaP2[1]:0)+(m_deltaP2[2]>0?m_deltaP2[2]:0);
		    aug2[2] = m_deltaP2[2]>0?m_deltaP2[2]:0;
		    
		    double summu = m_inimu[0] + m_inimu[1] + m_inimu[2];
 		    double sumxi = m_inixi[0] + m_inixi[1] + m_inixi[2];
		    for (int i = 0; i<3; i++)
	  	    {
                        m_nextpower[i] = m_inipower[i] - eta*(-m_inilambda[i]-summu*delta_time)
					+ sumxi*delta_time - rho*m_inideltaP[i] - rho*aug1[i] + rho*aug2[i];
  			summu-=m_inimu[i];
			sumxi-=m_inixi[i];
			if (m_nextpower[i]>P_max_desd)
			{
			    m_nextpower[i]=P_max_desd;
			} 
			else if (m_nextpower[i]<P_min_desd)
			{
			    m_nextpower[i] = P_min_desd;
			}
		    }
		    //copy m_nextpower to m_inipower for next iteration
		    for(int i = 0 ; i<3; i++)
		    {
			m_inipower[i] = m_nextpower[i];
		    }
	
		    double sumpower = 0.0;
		    for(int i = 0; i<3; i++)
		    {
			sumpower+=m_nextpower[i];
			m_deltaP1[i] = E_init[i]-E_full[i]-sumpower*delta_time;
			m_deltaP2[i] = sumpower*delta_time - E_init[i];
		    }	

		    for(int i = 0; i<3; i++)
	            {
			int temp = m_inimu[i]+eta*m_deltaP1[i];
			m_nextmu[i] = temp>0?temp:0;
			temp = m_inixi[i]+eta*m_deltaP2[i];
			m_nextxi[i] = temp>0?temp:0;
		    }

		    //copy m_nextmu, m_nextxi to m_inimu, m_inixi for next iteration
		    for(int i = 0; i<3; i++)
		    {
			m_inimu[i]=m_nextmu[i];
			m_inixi[i]=m_nextxi[i];
		    }

                    deltaPLambdaUpdate();
		    m_adjnum = m_localadj.size();
		    m_iteration++;
		    if (m_iteration < 5000)
		    {
		        sendtoAdjList();
		    }
		    else
		    {
			Logger.Status << "The DESD node" << m_localsymbol << " has power settings: " << m_nextpower[0]
				      << " " << m_nextpower[1] << " " << m_nextpower[2] << std::endl; 
		    }
		}
		//Grid updates
		else if(m_localsymbol == "1")
		{
		    double cost = 0.0;
		    for (int i = 0; i<3; i++)
		    {
			m_nextpower[i] = m_inipower[i] - eta*(price_profile[i]-m_inilambda[i]-rho*m_inideltaP[i]);
			if (m_nextpower[i] > P_max_grid)
			{
			    m_nextpower[i] = P_max_grid;
			}
			else if(m_nextpower[i]<P_min_grid)
			{
			    m_nextpower[i] = P_min_grid;
			}
			cost += price_profile[i]*m_inipower[i]*delta_time;
		    }
		    //copy m_nextpower to m_inipower for next iteration
		    for(int i = 0; i<3; i++)
		    {
			m_inipower[i] = m_nextpower[i];
		    }
                    Logger.Status << "The cost is " << cost << std::endl;
                    deltaPLambdaUpdate();
                    m_adjnum = m_localadj.size();
 		    m_iteration++;
  		    if (m_iteration < 5000)
		    {	
		        sendtoAdjList();
		    }
		    else
		    {
			Logger.Status << "The grid has power settings: " << m_nextpower[0] << " "
				      << m_nextpower[1] << " " << m_nextpower[2] << std::endl;
			Logger.Status << "The final cost is " << cost << std::endl;
		    }
		}
		//Other devices update
		else
		{
                    deltaPLambdaUpdate();
                    m_adjnum = m_localadj.size();
                    m_iteration++;
		    if (m_iteration < 5000)
		    {
                        sendtoAdjList();
		    }
                }
		for(int i = 0; i<3; i++)
		{
		    m_adjdeltaP[i] = 0.0;
		    m_adjlambda[i] = 0.0;
		}
            }
        }
    }
}


void DDAAgent::sendtoAdjList()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    //iteration, vsymbol, deltaP and lambda will be sent to adjacent list
    ModuleMessage mm;
    DesdStateMessage *msg = mm.mutable_desd_state_message();
    msg->set_iteration(m_iteration);
    msg->set_symbol(m_localsymbol);
    msg->set_deltapstep1(m_inideltaP[0]);
    msg->set_deltapstep2(m_inideltaP[1]);
    msg->set_deltapstep3(m_inideltaP[2]);
    msg->set_lambdastep1(m_inilambda[0]);
    msg->set_lambdastep2(m_inilambda[1]);
    msg->set_lambdastep3(m_inilambda[2]);
    Logger.Debug << "The message has been packed for sending to neighbors" << std::endl;

    //send message to adjecent list

    BOOST_FOREACH(std::string symbolid, m_localadj)
    {
        if (m_strans.find(symbolid) != m_strans.end())
        {
            std::string id = m_strans.find(symbolid)->second;
	    Logger.Debug << "The ID for adjacent node is " << id << std::endl;
            CPeerNode peer = CGlobalPeerList::instance().GetPeer(id);
            peer.Send(PrepareForSending(*msg));
        }
    }
}

ModuleMessage DDAAgent::PrepareForSending(const DesdStateMessage& message, std::string recipient)
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    ModuleMessage mm;
    mm.mutable_desd_state_message()->CopyFrom(message);
    mm.set_recipient_module(recipient);
    return mm;
}


void DDAAgent::deltaPLambdaUpdate()
{
    Logger.Trace << __PRETTY_FUNCTION__ << std::endl;
    //update delatP and lambda based on the iteration by locally or by adjacent list nodes
    if (m_iteration%inner_iter == 0 )
    {
        for (int i = 0; i < 3; i++)
        {
             m_nextdeltaP[i] = m_localratio*m_inideltaP[i] + m_adjratio*m_adjdeltaP[i]+ m_inideltaP[i] - m_nextdeltaP[i];    
	     m_nextlambda[i] = m_localratio*m_inilambda[i] + m_adjratio*m_adjdeltaP[i]+ eta*m_inideltaP[i];
        }
    }
    else
    {
        for (int i = 0; i<3; i++)
        {
             m_nextdeltaP[i] = m_inideltaP[i] + m_inideltaP[i] - m_nextdeltaP[i];
             m_nextlambda[i] = m_inilambda[i] + eta*m_inideltaP[i];
        }
    }
    //assign updated delatP and lambda to initial one for the iteration
    for (int i = 0; i<3; i++)
    {
        m_inideltaP[i]=m_nextdeltaP[i];
        m_inilambda[i]=m_nextlambda[i];
    }

}


} // namespace dda
} // namespace broker
} // namespace freedm


