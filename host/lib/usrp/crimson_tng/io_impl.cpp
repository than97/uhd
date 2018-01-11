//
// Copyright 2010-2012 Ettus Research LLC
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

#include <iomanip>
#include <iostream>

#include <boost/bind.hpp>
#include <boost/ref.hpp>

#include "uhd/stream.hpp"
<<<<<<< HEAD
#include "uhd/transport/udp_stream_zero_copy.hpp"
=======


>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0

#include "../../transport/super_recv_packet_handler.hpp"

#include "crimson_tng_impl.hpp"

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;

<<<<<<< HEAD
=======
/*
static managed_recv_buffer::sptr get_recv_buff_nop( double timeout ) {
	std::cout
		<< __FILE__ << ": " << __func__ << "(): " << __LINE__ << ":"
		<< std::endl;
	return
}
*/

static void issue_stream_command_nop( const uhd::stream_cmd_t &stream_cmd ) {
	std::cout
		<< __FILE__ << ": " << __func__ << "(): " << __LINE__ << ":"
		<< "would issue stream cmd { "
		<< "stream_mode: " << (char) stream_cmd.stream_mode << ", "
		<< "stream_now: " << stream_cmd.stream_now << ", "
		<< "time_spec: " << std::setprecision( 6 ) << stream_cmd.time_spec.get_real_secs() << " }"
		<< std::endl;
}

>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0
/***********************************************************************
 * Receive streamer
 **********************************************************************/
rx_streamer::sptr crimson_tng_impl::get_rx_stream(const uhd::stream_args_t &args_){
    stream_args_t args = args_;

    //setup defaults for unspecified values
<<<<<<< HEAD
    args.otw_format = "sc16";
    args.channels = args.channels.empty()? std::vector<size_t>(1, 0) : args.channels;
    _rx_channels = args.channels;
	_stream_cmd_samples_remaining = std::vector<size_t>( args.channels.size(), 0 );
=======
    args.otw_format = args.otw_format.empty()? "sc16" : args.otw_format;
    args.channels = args.channels.empty()? std::vector<size_t>(1, 0) : args.channels;
>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0

    //calculate packet size
    static const size_t hdr_size = 0
        + vrt::max_if_hdr_words32*sizeof(boost::uint32_t)
        + sizeof(vrt::if_packet_info_t().tlr) //forced to have trailer
        - sizeof(vrt::if_packet_info_t().cid) //no class id ever used
        - sizeof(vrt::if_packet_info_t().tsi) //no int time ever used
    ;
    const size_t bpp = 9000 - hdr_size;
    const size_t bpi = convert::get_bytes_per_item(args.otw_format);
    const size_t spp = unsigned(args.args.cast<double>("spp", bpp/bpi));

    const fs_path mb_path   = "/mboards/0";
	const fs_path link_path = mb_path / "rx_link";

<<<<<<< HEAD
    zero_copy_xport_params zcxp;
    udp_zero_copy::buff_params bp;

#ifndef DEFAULT_NUM_FRAMES
#define DEFAULT_NUM_FRAMES 32
#endif

    zcxp.send_frame_size = 0;
    zcxp.recv_frame_size = bpp;
    zcxp.num_send_frames = 0;
    zcxp.num_recv_frames = DEFAULT_NUM_FRAMES;

    set_properties_from_addr();
=======
    const zero_copy_xport_params zcxp = {
        .recv_frame_size = bpp,
        .send_frame_size = 0,
        .num_recv_frames = 5,
        .num_send_frames = 0,
    };

    udp_zero_copy::buff_params bp = {
        .recv_buff_size = bpp,
        .send_buff_size = 0,
    };

>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0

    rx_if = std::vector<uhd::transport::udp_zero_copy::sptr>( args.channels.size() );
    for( size_t i = 0; i < rx_if.size(); i++ ) {
		// get the channel parameters
		std::string ch       = boost::lexical_cast<std::string>((char)(args.channels[i] + 'A'));
		std::string udp_port = _tree->access<std::string>(link_path / "Channel_"+ch / "port").get();
		std::string ip_addr  = _tree->access<std::string>(link_path / "Channel_"+ch / "ip_dest").get();
		std::string iface    = _tree->access<std::string>(link_path / "Channel_"+ch / "iface").get();

<<<<<<< HEAD
		// power on the channel
		_tree->access<std::string>(mb_path / "rx" / "Channel_"+ch / "pwr").set("1");
		usleep(500000);

		// vita enable
		_tree->access<std::string>(link_path / "Channel_"+ch / "vita_en").set("1");

		rx_if[ i ] = udp_stream_zero_copy::make( ip_addr, udp_port, "127.0.0.1", "1", zcxp, bp, _addr );
=======
		rx_if[ i ] = uhd::transport::udp_zero_copy::make( ip_addr, udp_port, zcxp, bp, _addr );
>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0
    }

    //make the new streamer given the samples per packet
    boost::shared_ptr<sph::recv_packet_streamer> my_streamer = boost::make_shared<sph::recv_packet_streamer>(spp);

    //init some streamer stuff
    my_streamer->resize(args.channels.size());
    my_streamer->set_vrt_unpacker(&vrt::if_hdr_unpack_be);

<<<<<<< HEAD
    rx_streamers.resize( args.channels.size() );

    //set the converter
    uhd::convert::id_type id;
    id.input_format = args.otw_format + "_item32_le";
=======
    //set the converter
    uhd::convert::id_type id;
    id.input_format = args.otw_format + "_item32_be";
>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0
    id.num_inputs = 1;
    id.output_format = args.cpu_format;
    id.num_outputs = 1;
    my_streamer->set_converter(id);

    //bind callbacks for the handler
    for ( size_t i = 0; i < args.channels.size(); i++ ) {
<<<<<<< HEAD
        my_streamer->set_xport_chan_get_buff(
			i,
			boost::bind(
				& zero_copy_if::get_recv_buff,
				// for non-static methods, the first argument is a reference to the instance.
				// the default behaviour is to copy arguments, and in this case, that's ok, because it's
				// a shared_ptr.
				rx_if[ i ],
				_1
			),
			true /*flush*/
		);
		my_streamer->set_issue_stream_cmd(
			i,
			boost::bind(
				& crimson_tng_impl::set_stream_cmd,
				// for non-static methods, the first argument is a reference to the instance.
				// the default behaviour is to copy arguments, and in this case, that's ok, because it's
				// a shared_ptr.
				this,
				std::to_string( i ),
				_1
			)
		);
		rx_streamers[ i ] = my_streamer; //store weak pointer
=======
        my_streamer->set_xport_chan_get_buff( i, boost::bind( &zero_copy_if::get_recv_buff, rx_if[ i ], _1 ), true /*flush*/);
		my_streamer->set_issue_stream_cmd( i, boost::bind( & crimson_tng_impl::set_stream_cmd, boost::ref( *this ), std::to_string( i ), _1 ) );
>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0
    }

    //set the packet threshold to be an entire socket buffer's worth
//    const size_t packets_per_sock_buff = size_t(50e6/_mbc[_mbc.keys().front()].rx_dsp_xports[0]->get_recv_frame_size());
//    my_streamer->set_alignment_failure_threshold( packets_per_sock_buff );

    //sets all tick and samp rates on this streamer
<<<<<<< HEAD
    for( auto & i: _rx_channels ) {

    	std::string path = (
			boost::format( "/mboards/0/rx_dsps/Channel_%c/rate/value" )
    		% ( (char)('A' + i) )
    	).str();

    	_tree->access<double>( path ).update();
    }
=======
//    this->update_rates();
>>>>>>> 41187eee4c3e34d4f6756edb74794bd43b6910e0

    return my_streamer;
}
