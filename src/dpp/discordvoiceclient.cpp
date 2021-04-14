#include <string>
#include <iostream>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <dpp/discordvoiceclient.h>
#include <dpp/cache.h>
#include <dpp/cluster.h>
#include <thread>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <zlib.h>

namespace dpp {

std::string external_ip;

/**
 * @brief Represents an RTP packet. Size should always be exactly 12.
 */
struct rtp_header {
	uint16_t constant;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;

	rtp_header(uint16_t _seq, uint32_t _ts, uint32_t _ssrc) : constant(htons(0x8078)), sequence(htons(_seq)), timestamp(htonl(_ts)), ssrc(htonl(_ssrc)) {
	}
};

bool DiscordVoiceClient::sodium_initialised = false;

DiscordVoiceClient::DiscordVoiceClient(dpp::cluster* _cluster, snowflake _channel_id, snowflake _server_id, const std::string &_token, const std::string &_session_id, const std::string &_host)
       : WSClient(_host.substr(0, _host.find(":")), _host.substr(_host.find(":") + 1, _host.length()), "/?v=4"),
	creator(_cluster),
	channel_id(_channel_id),
	server_id(_server_id),
	token(_token),
	last_heartbeat(time(NULL)),
	heartbeat_interval(0),
	sessionid(_session_id),
	runner(nullptr),
	terminating(false),
	fd(-1),
	secret_key(nullptr),
	sequence(0),
	timestamp(0),
	sending(false)
{
#if HAVE_VOICE
	if (!DiscordVoiceClient::sodium_initialised) {
		if (sodium_init() < 0) {
			throw std::runtime_error("DiscordVoiceClient::DiscordVoiceClient; sodium_init() failed");
		}
		int opusError = 0;
		encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &opusError);
		if (opusError) {
			throw std::runtime_error(fmt::format("DiscordVoiceClient::DiscordVoiceClient; opus_encoder_create() failed: {}", opusError));
		}
		repacketizer = opus_repacketizer_create();
		external_ip = utility::external_ip();
		DiscordVoiceClient::sodium_initialised = true;
	}
	Connect();
#endif
}

DiscordVoiceClient::~DiscordVoiceClient()
{
	if (runner) {
		runner->join();
		delete runner;
	}
#if HAVE_VOICE
	if (encoder) {
		opus_encoder_destroy(encoder);
	}
#endif
	if (secret_key) {
		delete[] secret_key;
	}
}

bool DiscordVoiceClient::IsReady() {
	return secret_key != nullptr;
}

void DiscordVoiceClient::ThreadRun()
{
	do {
		SSLClient::ReadLoop();
		SSLClient::close();
		if (!terminating) {
			SSLClient::Connect();
			WSClient::Connect();
		}
	} while(!terminating);
}

void DiscordVoiceClient::Run()
{
	this->runner = new std::thread(&DiscordVoiceClient::ThreadRun, this);
	this->thread_id = runner->native_handle();
}

int DiscordVoiceClient::UDPSend(const char* data, size_t length)
{
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(this->port);
	servaddr.sin_addr.s_addr = inet_addr(this->ip.c_str());
	return sendto(this->fd, data, length, 0, (const struct sockaddr*)&servaddr, sizeof(sockaddr_in));
}

int DiscordVoiceClient::UDPRecv(char* data, size_t max_length)
{
	struct sockaddr sa;
	socklen_t sl;
	return recvfrom(this->fd, data, max_length, 0, (struct sockaddr*)&sa, &sl);
}

bool DiscordVoiceClient::HandleFrame(const std::string &data)
{
	log(dpp::ll_trace, fmt::format("R: {}", data));
	json j;
	
	try {
		j = json::parse(data);
	}
	catch (const std::exception &e) {
		log(dpp::ll_error, fmt::format("DiscordVoiceClient::HandleFrame {} [{}]", e.what(), data));
		return true;
	}

	if (j.find("op") != j.end()) {
		uint32_t op = j["op"];

		std::cout << data << "\n";

		switch (op) {
			/* Voice resume */
			case 9:
				log(ll_debug, "Voice connection resumed");
			break;
			/* Voice HELLO */
			case 8: {
				if (j.find("d") != j.end() && j["d"].find("heartbeat_interval") != j["d"].end() && !j["d"]["heartbeat_interval"].is_null()) {
					this->heartbeat_interval = j["d"]["heartbeat_interval"].get<uint32_t>();
				}

				if (modes.size()) {
					log(dpp::ll_debug, "Resuming voice session...");
						json obj = {
						{ "op", 7 },
						{
							"d",
							{
								{ "server_id", std::to_string(this->server_id) },
								{ "session_id", this->sessionid },
								{ "token", this->token },
							}
						}
					};
					this->write(obj.dump());
				} else {
					log(dpp::ll_debug, "Connecting new voice session...");
						json obj = {
						{ "op", 0 },
						{
							"d",
							{
								{ "user_id", creator->me.id },
								{ "server_id", std::to_string(this->server_id) },
								{ "session_id", this->sessionid },
								{ "token", this->token },
							}
						}
					};
					this->write(obj.dump());
				}
				this->connect_time = time(NULL);
			}
			break;
			/* Session description */
			case 4: {
				json &d = j["d"];
				secret_key = new uint8_t[32];
				size_t ofs = 0;
				for (auto & c : d["secret_key"]) {
					*(secret_key + ofs) = (uint8_t)c;
					ofs++;
					if (ofs > 31) {
						break;
					}
				}
				if (creator->dispatch.voice_ready) {
					voice_ready_t rdy(data);
					rdy.voice_client = this;
					creator->dispatch.voice_ready(rdy);
				}
			}
			break;
			/* Voice ready */
			case 2: {
				/* Video stream stuff comes in this frame too, but we can't use it (YET!) */
				json &d = j["d"];
				this->ip = d["ip"].get<std::string>();
				this->port = d["port"].get<uint16_t>();
				this->ssrc = d["ssrc"].get<uint64_t>();
				// Modes
				for (auto & m : d["modes"]) {
					this->modes.push_back(m.get<std::string>());
				}
				log(ll_debug, fmt::format("Voice websocket established; UDP endpoint: {}:{} [ssrc={}] with {} modes", ip, port, ssrc, modes.size()));

				int newfd = -1;
				if ((newfd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {

					sockaddr_in servaddr;
					memset(&servaddr, 0, sizeof(sockaddr_in));
					servaddr.sin_family = AF_INET;
					servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
					servaddr.sin_port = htons(0);

					if (bind(newfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
						throw std::runtime_error("Can't bind() client UDP socket");
					}
					
#ifdef _WIN32
					u_long mode = 1;
					int result = ioctlsocket(newfd, FIONBIO, &mode);
					if (result != NO_ERROR)
						throw std::runtime_error("Can't switch socket to non-blocking mode!");
#else
					int ofcmode;
					ofcmode = fcntl(newfd, F_GETFL, 0);
					ofcmode |= O_NDELAY;
					if (fcntl(newfd, F_SETFL, ofcmode)) {
						throw std::runtime_error("Can't switch socket to non-blocking mode!");
					}
#endif
					/* Hook select() in the SSLClient to add a new file descriptor */
					this->fd = newfd;
					this->custom_writeable_fd = std::bind(&DiscordVoiceClient::WantWrite, this);
					this->custom_readable_fd = std::bind(&DiscordVoiceClient::WantRead, this);
					this->custom_writeable_ready = std::bind(&DiscordVoiceClient::WriteReady, this);
					this->custom_readable_ready = std::bind(&DiscordVoiceClient::ReadReady, this);

					int bound_port = 0;
					struct sockaddr_in sin;
					socklen_t len = sizeof(sin);
					if (getsockname(this->fd, (struct sockaddr *)&sin, &len) > -1) {
						bound_port = ntohs(sin.sin_port);
					}

					this->write(json({
						{ "op", 1 },
							{ "d", {
								{ "protocol", "udp" },
								{ "data", {
										{ "address", external_ip },
										{ "port", bound_port },
										{ "mode", "xsalsa20_poly1305" }
									}
								}
							}
						}
					}).dump());
				}
			}
			break;
		}
	}
	return true;
}

void DiscordVoiceClient::Send(const char* packet, size_t len) {
	outbuf.push_back(std::string(packet, len));
}

void DiscordVoiceClient::ReadReady()
{
	/* read from udp into buffer tail (push_back) -- XXX FIXME */
	char buffer[10];
	int r = this->UDPRecv(buffer, sizeof(buffer));
}

void DiscordVoiceClient::WriteReady()
{
	if (outbuf.size()) {
		if (this->UDPSend(outbuf[0].data(), outbuf[0].length()) == outbuf[0].length()) {
			outbuf.erase(outbuf.begin());
			if (creator->dispatch.voice_buffer_send) {
				voice_buffer_send_t snd("");
				snd.buffer_size = outbuf.size();
				snd.voice_client = this;
				creator->dispatch.voice_buffer_send(snd);
			}
		}
	}
}

dpp::utility::uptime DiscordVoiceClient::Uptime()
{
	return dpp::utility::uptime(time(NULL) - connect_time);
}

bool DiscordVoiceClient::IsConnected()
{
	return (this->GetState() == CONNECTED);
}

int DiscordVoiceClient::WantWrite() {
	if (outbuf.size()) {
		return fd;
	} else {
		return -1;
	}
}

int DiscordVoiceClient::WantRead() {
	return fd;
}

void DiscordVoiceClient::Error(uint32_t errorcode)
{
	std::map<uint32_t, std::string> errortext = {
		{ 1000, "Socket shutdown" },
		{ 1001, "Client is leaving" },
		{ 1002, "Endpoint received a malformed frame" },
		{ 1003, "Endpoint received an unsupported frame" },
		{ 1004, "Reserved code" },
		{ 1005, "Expected close status, received none" },
		{ 1006, "No close code frame has been receieved" },
		{ 1007, "Endpoint received inconsistent message (e.g. malformed UTF-8)" },
		{ 1008, "Generic error" },
		{ 1009, "Endpoint won't process large frame" },
		{ 1010, "Client wanted an extension which server did not negotiate" },
		{ 1011, "Internal server error while operating" },
		{ 1012, "Server/service is restarting" },
		{ 1013, "Temporary server condition forced blocking client's request" },
		{ 1014, "Server acting as gateway received an invalid response" },
		{ 1015, "Transport Layer Security handshake failure" },
		{ 4001, "Unknown opcode" },
		{ 4002, "Failed to decode payload" },
		{ 4003, "Not authenticated" },
		{ 4004, "Authentication failed" },
		{ 4005, "Already authenticated" },
		{ 4006, "Session no longer valid" },
		{ 4009, "Session timeout" },
		{ 4011, "Server not found" },
		{ 4012, "Unknown protocol" },
		{ 4014, "Disconnected" },
		{ 4015, "Voice server crashed" },
		{ 4016, "Unknown encryption mode" }
	};
	std::string error = "Unknown error";
	auto i = errortext.find(errorcode);
	if (i != errortext.end()) {
		error = i->second;
	}
	log(dpp::ll_warning, fmt::format("OOF! Error from underlying websocket: {}: {}", errorcode, error));
}

void DiscordVoiceClient::log(dpp::loglevel severity, const std::string &msg)
{
	creator->log(severity, msg);
}

void DiscordVoiceClient::QueueMessage(const std::string &j, bool to_front)
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	if (to_front) {
		message_queue.push_front(j);
	} else {
		message_queue.push_back(j);
	}
}

void DiscordVoiceClient::ClearQueue()
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	message_queue.clear();
}

size_t DiscordVoiceClient::GetQueueSize()
{
	std::lock_guard<std::mutex> locker(queue_mutex);
	return message_queue.size();
}

void DiscordVoiceClient::OneSecondTimer()
{
	/* Rate limit outbound messages, 1 every odd second, 2 every even second */
	if (this->GetState() == CONNECTED) {
		for (int x = 0; x < (time(NULL) % 2) + 1; ++x) {
			std::lock_guard<std::mutex> locker(queue_mutex);
			if (message_queue.size()) {
				std::string message = message_queue.front();
				message_queue.pop_front();
				this->write(message);
			}
		}

		if (this->heartbeat_interval) {
			/* Check if we're due to emit a heartbeat */
			if (time(NULL) > last_heartbeat + ((heartbeat_interval / 1000.0) * 0.75)) {
				log(dpp::ll_debug, "Voice: Emit heartbeat");
				QueueMessage(json({{"op", 3}, {"d", 12345678}}).dump(), true);
				last_heartbeat = time(NULL);
			}
		}
	}
}

size_t DiscordVoiceClient::encode(uint8_t *input, size_t inDataSize, uint8_t *output, size_t &outDataSize)
{
  outDataSize = 0;
  int mEncFrameBytes = 11520;
  int mEncFrameSize = 2880;
  if (0 == (inDataSize % mEncFrameBytes)) {
    bool isOk = true;
    size_t cur = 0;
    uint8_t *out = encode_buffer;

    memset(out, 0, sizeof(encode_buffer));
    repacketizer = opus_repacketizer_init(repacketizer);
    for (size_t i = 0; i < (inDataSize / mEncFrameBytes); ++ i) {
      const opus_int16* pcm = (opus_int16*)(input + i * mEncFrameBytes);
      int ret = opus_encode(encoder, pcm, mEncFrameSize, out, 65536);
      if (ret > 0) {
        int retval = opus_repacketizer_cat(repacketizer, out, ret);
        if (retval != OPUS_OK) {
          isOk = false;
	  log(ll_warning, fmt::format("opus_repacketizer_cat(): {}", opus_strerror(retval)));
          break;
        }
        out += ret;
	cur += ret;
      } else {
        isOk = false;
	log(ll_warning, fmt::format("opus_encode(): {}", opus_strerror(ret)));
        break;
      }
    }
    if (isOk) {
      int ret = opus_repacketizer_out(repacketizer, output, 65536);
      if (ret > 0) {
        outDataSize = ret;
      } else {
	log(ll_warning, fmt::format("opus_repacketizer_out(): {}", opus_strerror(ret)));
      }
    }
  } else {
      throw std::runtime_error(fmt::format("Invalid input data length: {}, must be n times of {}", inDataSize, mEncFrameBytes));
  }
  return outDataSize;
}

void DiscordVoiceClient::SendAudio(uint16_t* audio_data, const size_t length, bool use_opus)  {
#if HAVE_VOICE

	const size_t max_frame_bytes = 11520;
	uint8_t pad[max_frame_bytes] = { 0 };
	if (length > max_frame_bytes && use_opus) {
		std::string s_audio_data((const char*)audio_data, length);
		while (s_audio_data.length() > max_frame_bytes) {
			std::string packet(s_audio_data.substr(0, max_frame_bytes));
			s_audio_data.erase(s_audio_data.begin(), s_audio_data.begin() + max_frame_bytes);
			if (packet.size() < max_frame_bytes) {
				packet.resize(max_frame_bytes, 0);
			}
			SendAudio((uint16_t*)packet.data(), max_frame_bytes, use_opus);
		}

		return;

	}


	int frameSize = 2880;
	opus_int32 encodedAudioMaxLength = length;
	uint8_t encodedAudioData[encodedAudioMaxLength];
	size_t encodedAudioLength = encodedAudioMaxLength;
	if (use_opus) {
		encodedAudioLength = this->encode((uint8_t*)audio_data, length, encodedAudioData, encodedAudioLength);
	} else {
	}

	++sequence;
	const int headerSize = 12;
	const int nonceSize = 24;
	rtp_header header(sequence, timestamp, ssrc);

	int8_t nonce[nonceSize];
	std::memcpy(nonce, &header, sizeof(header));
	std::memset(nonce + sizeof(header), 0, sizeof(nonce) - sizeof(header));

	std::vector<uint8_t> audioDataPacket(sizeof(header) + encodedAudioLength + crypto_secretbox_MACBYTES);
	std::memcpy(audioDataPacket.data(), &header, sizeof(header));

	crypto_secretbox_easy(audioDataPacket.data() + sizeof(header), encodedAudioData, encodedAudioLength, (const unsigned char*)nonce, secret_key);

	Send((const char*)audioDataPacket.data(), audioDataPacket.size());
	timestamp += frameSize;

	if (!this->sending) {		
		this->QueueMessage(json({
		{"op", 5},
		{"d", {
			{"speaking", 1},
			{"delay", 0},
			{"ssrc", ssrc}
		}}
		}).dump(), true);
		sending = true;
	}

#endif
}



};