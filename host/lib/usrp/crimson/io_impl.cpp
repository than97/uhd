//
// Copyright 2014-2015 Per Vices
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "crimson_impl.hpp"
#include "crimson_fw_common.h"
#include <uhd/utils/log.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/tasks.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/byteswap.hpp>
#include <uhd/utils/thread_priority.hpp>
#include <uhd/transport/bounded_buffer.hpp>
#include <uhd/transport/udp_stream.hpp>
#include <uhd/types/wb_iface.hpp>
#include <boost/thread/thread.hpp>
#include <boost/format.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/make_shared.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;
namespace asio = boost::asio;
namespace pt = boost::posix_time;

class crimson_rx_streamer : public uhd::rx_streamer {
public:
	crimson_rx_streamer(device_addr_t addr, property_tree::sptr tree, std::vector<size_t> channels) {
		init_rx_streamer(addr, tree, channels);
	}

	crimson_rx_streamer(device_addr_t addr, property_tree::sptr tree) {
		init_rx_streamer(addr, tree, std::vector<size_t>(1, 0));
	}

	~crimson_rx_streamer() {
	}

	// number of channels for streamer
	size_t get_num_channels(void) const {
		return _channels.size();
	}

	// max samples per buffer per packet for sfpa, (4-bytes: 16 I, 16Q)
	size_t get_max_num_samps(void) const {
		return _pay_len/4;
	}

	size_t recv(
        	const buffs_type &buffs,
        	const size_t nsamps_per_buff,
        	rx_metadata_t &metadata,
        	const double timeout = 0.1,
        	const bool one_packet = true )
	{
		const size_t vita_hdr = 4;
		const size_t vita_tlr = 1;
		const size_t vita_pck = nsamps_per_buff + vita_hdr + vita_tlr;
		size_t nbytes = 0;

		// temp buffer: vita hdr + data
		uint32_t vita_buf[vita_pck];

		// read from each connected stream and dump it into the buffs[i]
		for (unsigned int i = 0; i < _channels.size(); i++) {

			// clear temp buffer and otuput buffer
			memset(vita_buf, 0, vita_pck * 4);
			memset(buffs[i], 0, nsamps_per_buff * 4);

			// read in vita_pck*4 bytes to temp buffer
			nbytes = _udp_stream[i] -> stream_in(vita_buf, vita_pck * 4, timeout);
			if (nbytes == 0) return 0;

			// copy non-vita packets to buffs[0]
			memcpy(buffs[i], vita_buf + vita_hdr , nsamps_per_buff * 4);
		}

		// process vita timestamps based on the last stream input's time stamp
		uint64_t time_ticks = ((uint64_t)vita_buf[2] << 32) | ((uint64_t)vita_buf[3]);

		// determine the beginning of time
		if (_start_ticks == 0) {
			_start_ticks = time_ticks;
		}

		// save the time to metadata
		time_ticks = time_ticks - _start_ticks;
        metadata.time_spec = time_spec_t::from_ticks(time_ticks, _rate);

		// process vita sequencing
		uint32_t header = vita_buf[0];
		if (_prev_frame > (header & 0xf0000) >> 16) {
			metadata.out_of_sequence = true;
		} else {
			metadata.out_of_sequence = false;
		}
		_prev_frame = (header & 0xf0000) >> 16;

		// populate metadata
		metadata.error_code = rx_metadata_t::ERROR_CODE_NONE;
		metadata.start_of_burst = true;		// valid for a one packet
		metadata.end_of_burst = true;		// valid for a one packet
		metadata.fragment_offset = 0;		// valid for a one packet
		metadata.more_fragments = false;	// valid for a one packet
		metadata.has_time_spec = true;		// valis for Crimson
		return (nbytes / 4) - 5;		// removed the 5 VITA 32-bit words
	}

	void issue_stream_cmd(const stream_cmd_t &stream_cmd) {
	}

private:
	// init function, common to both constructors
	void init_rx_streamer(device_addr_t addr, property_tree::sptr tree, std::vector<size_t> channels) {
		// save the tree
		_tree = tree;
		_channels = channels;
		_prev_frame = 0;
		_start_ticks = 0;

		// get the property root path
		const fs_path mb_path   = "/mboards/0";
		const fs_path link_path = mb_path / "rx_link";

		// if no channels specified, default to channel 1 (0)
		_channels = _channels.empty() ? std::vector<size_t>(1, 0) : _channels;

		for (unsigned int i = 0; i < _channels.size(); i++) {
			// get the channel parameters
			std::string ch       = boost::lexical_cast<std::string>((char)(_channels[i] + 65));
			std::string udp_port = tree->access<std::string>(link_path / "Channel_"+ch / "port").get();
			std::string ip_addr  = tree->access<std::string>(link_path / "Channel_"+ch / "ip_dest").get();
			std::string iface    = tree->access<std::string>(link_path / "Channel_"+ch / "iface").get();
			_rate = tree->access<double>(mb_path / "rx_dsps" / "Channel_"+ch / "rate" / "value").get();
			_pay_len = tree->access<int>(mb_path / "link" / iface / "pay_len").get();

			// power on the channel
			tree->access<std::string>(mb_path / "rx" / "Channel_"+ch / "pwr").set("1");
			sleep(5);

			// vita enable
			tree->access<std::string>(link_path / "Channel_"+ch / "vita_en").set("1");

			// connect to UDP port
			_udp_stream.push_back(uhd::transport::udp_stream::make_rx_stream( ip_addr, udp_port));
		}
	}

	// helper function to convert 8-bit allignment ==> 32-bit allignment
	void _32_align(uint32_t* data) {
		*data = (*data & 0x000000ff) << 24 |
			(*data & 0x0000ff00) << 8  |
			(*data & 0x00ff0000) >> 8  |
			(*data & 0xff000000) >> 24;
	}

	std::vector<uhd::transport::udp_stream::sptr> _udp_stream;
	std::vector<size_t> _channels;
	property_tree::sptr _tree;
	size_t _prev_frame;
	size_t _pay_len;
	double _rate;
	uint64_t _start_ticks;
};

class crimson_tx_streamer : public uhd::tx_streamer {
public:
	crimson_tx_streamer(device_addr_t addr, property_tree::sptr tree, std::vector<size_t> channels, boost::mutex* udp_mutex_add) {
		init_tx_streamer(addr, tree, channels, udp_mutex_add);
	}

	crimson_tx_streamer(device_addr_t addr, property_tree::sptr tree, boost::mutex* udp_mutex_add) {
		init_tx_streamer(addr, tree, std::vector<size_t>(1, 0), udp_mutex_add);
	}

	~crimson_tx_streamer() {
	}

	// number of channels for streamer
	size_t get_num_channels(void) const {
		return _channels.size();
	}

	// max samples per buffer per packet for sfpa, (4-bytes: 16 I, 16Q)
	size_t get_max_num_samps(void) const {
		return _pay_len/4;
	}

	size_t send(
        	const buffs_type &buffs,
        	const size_t nsamps_per_buff,
        	const tx_metadata_t &metadata,
        	const double timeout = 0.1)
	{
		const size_t vita_pck = nsamps_per_buff;// + vita_hdr + vita_tlr;	// vita is disabled
		uint32_t vita_buf[vita_pck];						// buffer to read in data plus room for VITA
		size_t ret;
		ret = 0;

		// send to each connected stream data in buffs[i]
		for (unsigned int i = 0; i < _channels.size(); i++) {

			// update sample rate if we don't know the sample rate
			if (_samp_rate[i] == 0) {
				//Get sample rate
				std::string ch = boost::lexical_cast<std::string>((char)(_channels[i] + 65));
				_samp_rate[i] = _tree->access<double>("/mboards/0/tx_dsps/Channel_"+ch+"/rate/value").get();
				//Set the user set sample rate to refer to later
				_samp_rate_usr[i] = _samp_rate[i];

				//Adjust sample rate to fill up buffer in first half second
				//we do this by setting setting the "last time " data was sent to be half a buffers worth in the past
				//each element in the buffer is 2 samples worth
				time_spec_t past_halfbuffer = time_spec_t(0, (_fifo_level_perc/100*(double)(CRIMSON_BUFF_SIZE*2)) / (double)_samp_rate[i]);
				_last_time[i] = time_spec_t::get_system_time()-past_halfbuffer;
			}

			//Flow control init
			//check if flow control is running, if not run it
			if (_flow_running == false)	boost::thread flowcontrolThread(init_flowcontrol,this);

			//Check if it is time to send data, if so, copy the data over and continue
			size_t remaining_bytes = (nsamps_per_buff*4);
			while (remaining_bytes >0){

				//If greater then max pl copy over what you can, leave the rest
				if (nsamps_per_buff*4 >=CRIMSON_MAX_MTU){
					//if( time_spec_t::get_system_time() - _last_time[i] >= wait) {
						//clear temp buffer (byte operation)
						memset((void*)vita_buf, 0, vita_pck*4);
						memcpy((void*)vita_buf, buffs[i], CRIMSON_MAX_MTU);


						std::cout  << std::setprecision(18)<< "B4 While Last: " <<_last_time[i].get_real_secs()<< "  Now: " <<time_spec_t::get_system_time().get_real_secs()<<std::endl;
						while ( time_spec_t::get_system_time() < _last_time[i]) {
							update_samplerate();
						}
						std::cout  << std::setprecision(18)<< "After While Last: " <<_last_time[i].get_real_secs()<< "  Now: " <<time_spec_t::get_system_time().get_real_secs()<<std::endl;


						std::cout  << std::setprecision(18)<< "Last Time: " <<_last_time[i].get_real_secs()<< std::endl;
						//Send data (byte operation)
						ret += _udp_stream[i] -> stream_out((void*)vita_buf + ret, CRIMSON_MAX_MTU);

						//DEBUG: Print out adjusted sample rate
						std::cout  << std::setprecision(18)<< "Last Time: " <<_last_time[i].get_real_secs()<< std::endl;

						//update last_time with when it was supposed to have been sent:
						time_spec_t wait = time_spec_t(0, (double)(CRIMSON_MAX_MTU / 4.0) / (double)_samp_rate[i]);

						std::cout  << std::setprecision(18)<< "Last Time: " <<_last_time[i].get_real_secs()<< std::endl;
						std::cout  << std::setprecision(18)<< "Wait: " <<wait.get_real_secs()<< std::endl;
						_last_time[i] = _last_time[i]+wait;//time_spec_t::get_system_time();
						std::cout  << std::setprecision(18)<< "New Last: " <<_last_time[i].get_real_secs()<< std::endl;

				}else{
					//if( time_spec_t::get_system_time() - _last_time[i] >= wait) {
						//clear temp buffer
						memset((void*)vita_buf, 0, vita_pck*4);
						memcpy((void*)vita_buf, buffs[i], remaining_bytes);


						while ( time_spec_t::get_system_time() < _last_time[i]) {
							update_samplerate();
						}
						//Send data (byte operation)
						ret += _udp_stream[i] -> stream_out((void*)vita_buf + ret, remaining_bytes);

						//update last_time with when it was supposed to have been sent:
						time_spec_t wait = time_spec_t(0, (double)(remaining_bytes/4) / (double)_samp_rate[i]);
						_last_time[i] = _last_time[i]+wait;//time_spec_t::get_system_time();
						//Send data (byte operation)

				}
				remaining_bytes = (nsamps_per_buff*4) - ret;
				//time_spec_t systime = time_spec_t::get_system_time();
				//double systime_real = systime.get_real_secs();
				//double last_time_real = _last_time[i].get_real_secs();
				//if (systime_real < last_time_real){
				//	boost::this_thread::sleep(boost::posix_time::milliseconds((last_time_real-systime_real)*1000));
				//}

			}

		}
		return (ret / 4);// -  vita_hdr - vita_tlr;	// vita is disabled
	}

	// async messages are currently disabled
	bool recv_async_msg( async_metadata_t &async_metadata, double timeout = 0.1) {
		return false;
	}

private:
	// init function, common to both constructors
	void init_tx_streamer( device_addr_t addr, property_tree::sptr tree, std::vector<size_t> channels,boost::mutex* udp_mutex_add) {
		// save the tree
		_tree = tree;
		_channels = channels;

		// setup the flow control UDP channel
    		_flow_iface = crimson_iface::make( udp_simple::make_connected(
		        addr["addr"], BOOST_STRINGIZE(CRIMSON_FLOW_CNTRL_UDP_PORT)) );

		// get the property root path
		const fs_path mb_path   = "/mboards/0";
		const fs_path prop_path = mb_path / "tx_link";

		// if no channels specified, default to channel 1 (0)
		_channels = _channels.empty() ? std::vector<size_t>(1, 0) : _channels;

		for (unsigned int i = 0; i < _channels.size(); i++) {
			std::string ch       = boost::lexical_cast<std::string>((char)(_channels[i] + 65));
			std::string udp_port = tree->access<std::string>(prop_path / "Channel_"+ch / "port").get();
			std::string iface    = tree->access<std::string>(prop_path / "Channel_"+ch / "iface").get();
			std::string ip_addr  = tree->access<std::string>( mb_path / "link" / iface / "ip_addr").get();
			_pay_len = tree->access<int>(mb_path / "link" / iface / "pay_len").get();

			// power on the channel
			tree->access<std::string>(mb_path / "tx" / "Channel_"+ch / "pwr").set("1");
			sleep(5);

			// vita disable
			tree->access<std::string>(prop_path / "Channel_"+ch / "vita_en").set("0");

			// connect to UDP port
			_udp_stream.push_back(uhd::transport::udp_stream::make_tx_stream(ip_addr, udp_port));

			//Launch thread for flow control

			//Set up initial flow control variables
			_flow_running=false;
			_buffer_count[0] = 0;
			_buffer_count[1] = 0;
			_udp_mutex_add = udp_mutex_add;

			// initialize sample rate
			_samp_rate.push_back(0);
			_samp_rate_usr.push_back(0);

			// initialize the _last_time
			_last_time.push_back(time_spec_t(0.0));
			_fifo_level_perc = 50;

		}
	}

	 // Flow Control (should be called once on seperate thread)
	static void init_flowcontrol(crimson_tx_streamer* txstream) {

		//Get flow control updates x times a second
		uint32_t wait = 1000/CRIMSON_UPDATE_PER_SEC;
		txstream->_flow_running = true;

		while(true){
			//get data under mutex lock
			txstream->_udp_mutex_add->lock();
			txstream->_flow_iface -> poke_str("Read fifo");
			std::string buff_read = txstream->_flow_iface -> peek_str();
			txstream->_udp_mutex_add->unlock();

			// remove the "flow," at the beginning of the string
			buff_read.erase(0, 5);

			//Prevent multiple access
			txstream->_flowcontrol_mutex.lock();

			// read in each fifo level, ignore() will skip the commas
			std::stringstream ss(buff_read);
			for (int j = 0; j < 4; j++) {
				ss >> txstream->_fifo_lvl[j];
				ss.ignore();
			}
			//increment buffer count to say we have data
			txstream->_buffer_count[0]++;

			//unlock
			txstream->_flowcontrol_mutex.unlock();


			//sleep for the designated update period
			boost::this_thread::sleep(boost::posix_time::milliseconds(wait));
		}



	}
	void update_samplerate(){
		for (unsigned int i = 0; i < _channels.size(); i++) {
			if(_buffer_count[0]!=_buffer_count[1]){
				//If mutex is locked, let the streamer loop around and try again if we are still waiting
				if(_flowcontrol_mutex.try_lock()){

					// calculate the error - aim for 50%
					_fifo_lvl[i] = ((CRIMSON_BUFF_SIZE*_fifo_level_perc/100)- _fifo_lvl[i]) / (CRIMSON_BUFF_SIZE);
					//apply correction
					_samp_rate[i]=_samp_rate[i]+(_fifo_lvl[i]*_samp_rate[i])/10000000;

					//Limit the correction
					//Maximum correction is a half buffer per second (a buffer element is 2 samples).
					if(_samp_rate[i] > (_samp_rate_usr[i] + CRIMSON_BUFF_SIZE)){
						_samp_rate[i] = _samp_rate_usr[i] + CRIMSON_BUFF_SIZE;
					}else if(_samp_rate[i] < (_samp_rate_usr[i] - CRIMSON_BUFF_SIZE)){
						_samp_rate[i] = _samp_rate_usr[i] - CRIMSON_BUFF_SIZE;
					}

					//Buffer is now handled
					_buffer_count[i+1] = _buffer_count[0];
					_flowcontrol_mutex.unlock();
				}
			}
		}

	}
	// helper function to swap bytes, within 32-bits
	void _32_align(uint32_t* data) {
		*data = (*data & 0x000000ff) << 24 |
			(*data & 0x0000ff00) << 8  |
			(*data & 0x00ff0000) >> 8  |
			(*data & 0xff000000) >> 24;
	}

	std::vector<uhd::transport::udp_stream::sptr> _udp_stream;
	std::vector<size_t> _channels;
	std::vector<double> _samp_rate;
	std::vector<double> _samp_rate_usr;
	std::vector<time_spec_t> _last_time;
	property_tree::sptr _tree;
	size_t _pay_len;
	uhd::wb_iface::sptr _flow_iface;
	boost::mutex _flowcontrol_mutex;
	double _fifo_lvl[4];
	uint32_t _buffer_count[2];
	bool _flow_running;
	boost::mutex* _udp_mutex_add;
	double _fifo_level_perc;
};

/***********************************************************************
 * Async Data
 **********************************************************************/
// async messages are currently disabled and are deprecated according to UHD
bool crimson_impl::recv_async_msg(
    async_metadata_t &async_metadata, double timeout
){
    return false;
}

/***********************************************************************
 * Receive streamer
 **********************************************************************/
rx_streamer::sptr crimson_impl::get_rx_stream(const uhd::stream_args_t &args){
	// Crimson currently only supports cpu_format of "sc16" (complex<int16_t>) stream
	if (strcmp(args.cpu_format.c_str(), "sc16") != 0 && strcmp(args.cpu_format.c_str(), "") != 0 ) {
		UHD_MSG(error) << "CRIMSON Stream only supports cpu_format of \
			\"sc16\" complex<int16_t>" << std::endl;
	}

	// Crimson currently only supports (over the wire) otw_format of "sc16" - Q16 I16 if specified
	if (strcmp(args.otw_format.c_str(), "sc16") != 0 && strcmp(args.otw_format.c_str(), "") != 0 ) {
		UHD_MSG(error) << "CRIMSON Stream only supports otw_format of \
			\"sc16\" Q16 I16" << std::endl;
	}

	// Warning for preference to set the MTU size to 3600 to support Jumbo Frames
        boost::format base_message (
            "\nCrimson Warning:\n"
            "   Please set the MTU size for SFP ports to 4000.\n"
            "   The device has been optimized for Jumbo Frames\n"
	    "   to lower overhead.\n");
	UHD_MSG(status) << base_message.str();

	// TODO firmware support for other otw_format, cpu_format
	return rx_streamer::sptr(new crimson_rx_streamer(this->_addr, this->_tree, args.channels));
}

/***********************************************************************
 * Transmit streamer
 **********************************************************************/
tx_streamer::sptr crimson_impl::get_tx_stream(const uhd::stream_args_t &args){
	// Crimson currently only supports cpu_format of "sc16" (complex<int16_t>) stream
	if (strcmp(args.cpu_format.c_str(), "sc16") != 0 && strcmp(args.cpu_format.c_str(), "") != 0 ) {
		UHD_MSG(error) << "CRIMSON Stream only supports cpu_format of \
			\"sc16\" complex<int16_t>" << std::endl;
	}

	// Crimson currently only supports (over the wire) otw_format of "sc16" - Q16 I16 if specified
	if (strcmp(args.otw_format.c_str(), "sc16") != 0 && strcmp(args.otw_format.c_str(), "") != 0 ) {
		UHD_MSG(error) << "CRIMSON Stream only supports otw_format of \
			\"sc16\" Q16 I16" << std::endl;
	}

	// Warning for preference to set the MTU size to 3600 to support Jumbo Frames
        boost::format base_message (
            "\nCrimson Warning:\n"
            "   Please set the MTU size for SFP ports to 4000 \n"
            "   The device has been optimized for Jumbo Frames\n"
	    "   to lower overhead.\n");
	UHD_MSG(status) << base_message.str();

	// TODO firmware support for other otw_format, cpu_format
	return tx_streamer::sptr(new crimson_tx_streamer(this->_addr, this->_tree, args.channels, &this->udp_mutex));
}
