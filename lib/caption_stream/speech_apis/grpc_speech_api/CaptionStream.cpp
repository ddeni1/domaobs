/******************************************************************************
Copyright (C) 2019 by <rat.with.a.compiler@gmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
******************************************************************************/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

#include <utility>
#include <string>
#include <sstream>
#include "CaptionStream.h"
#include "utils.h"
#include "log.h"

#include <grpcpp/grpcpp.h>
#include <yandex/cloud/ai/stt/v3/stt_service.grpc.pb.h>
#include <yandex/cloud/ai/stt/v3/stt.pb.h>

using speechkit::stt::v3::Recognizer;
using speechkit::stt::v3::StreamingRequest;
using speechkit::stt::v3::StreamingResponse;
using speechkit::stt::v3::StreamingOptions;
using speechkit::stt::v3::RecognitionModelOptions;
using speechkit::stt::v3::TextNormalizationOptions;
using speechkit::stt::v3::AudioFormatOptions;
using speechkit::stt::v3::RawAudio;
using speechkit::stt::v3::LanguageRestrictionOptions;
using speechkit::stt::v3::AudioChunk;

static void audio_sender_thread(std::shared_ptr<CaptionStream> self);

static void _audio_sender(CaptionStream &self);

static void read_results_loop_thread(
        CaptionStream &self,
        grpc::ClientReaderWriterInterface<StreamingRequest, StreamingResponse> *streamer
);

CaptionStream::CaptionStream(
        const CaptionStreamSettings settings
) :
        settings(settings),
        session_pair(random_string(15)) {
    info_log("CaptionStream YANDEX SpeechKit v3, created session pair: %s", session_pair.c_str());
}

bool CaptionStream::start(std::shared_ptr<CaptionStream> self) {
    if (self.get() != this)
        return false;

    if (started)
        return false;

    started = true;
    thread upstream_thread(audio_sender_thread, self);
    upstream_thread.detach();
    return true;
}


static void audio_sender_thread(std::shared_ptr<CaptionStream> self) {
    info_log("starting audio_sender_thread() thread");
    if (!self) {
        self->stop();
        info_log("audio_sender_thread() no self");
        return;
    }

    _audio_sender(*self);
    self->stop();
    info_log("finished audio_sender_thread() thread");
}

static bool write_config(
        grpc::ClientReaderWriterInterface<StreamingRequest, StreamingResponse> *streamer,
        const CaptionStreamSettings &settings
) {
    StreamingRequest request;
    auto *session_options = request.mutable_session_options();
    auto *rec_model = session_options->mutable_recognition_model();
    rec_model->set_model("general");
    rec_model->set_audio_processing_type(RecognitionModelOptions::REAL_TIME);

    auto *audio_format = rec_model->mutable_audio_format();
    auto *raw_audio = audio_format->mutable_raw_audio();
    raw_audio->set_audio_encoding(RawAudio::LINEAR16_PCM);
    raw_audio->set_sample_rate_hertz(16000);
    raw_audio->set_audio_channel_count(1);

    // === Это главное — выключаем "литературизацию" ===
    auto *text_norm = rec_model->mutable_text_normalization();
    text_norm->set_text_normalization(TextNormalizationOptions::TEXT_NORMALIZATION_ENABLED);
    text_norm->set_profanity_filter(bool(settings.profanity_filter));
    text_norm->set_literature_text(false);  // <-- НЕ литературизировать

    // Language restriction — say it's Russian (or whatever user picked)
    auto *lang_restriction = rec_model->mutable_language_restriction();
    lang_restriction->set_restriction_type(LanguageRestrictionOptions::WHITELIST);
    lang_restriction->add_language_code(settings.language.empty() ? "ru-RU" : settings.language);

    return streamer->Write(request);
}

static void write_audio_loop(
        grpc::ClientReaderWriterInterface<StreamingRequest, StreamingResponse> *streamer,
        CaptionStream &self
) {
    uint chunk_count = 0;
    StreamingRequest request;

    while (!self.is_stopped()) {
        string *audio_chunk = self.dequeue_audio_data(self.settings.send_timeout_ms * 1000);
        if (audio_chunk == nullptr) {
            info_log("couldn't deque audio chunk in time");
            break;
        }

        if (audio_chunk->empty()) {
            info_log("got 0 size audio chunk. ignoring");
            delete audio_chunk;
            continue;
        }

        // Yandex API uses an oneof Event with chunk
        request.Clear();
        auto *chunk = request.mutable_chunk();
        chunk->set_data(*audio_chunk);

        if (!streamer->Write(request)) {
            info_log("write_audio_loop write failed, stopping");
            delete audio_chunk;
            break;
        }
        if (chunk_count % 20 == 0)
            info_log("sent audio chunk %d, %lu bytes", chunk_count, audio_chunk->size());

        delete audio_chunk;
        chunk_count++;
    }

}

static void read_results_loop_thread(
        CaptionStream &self,
        grpc::ClientReaderWriterInterface<StreamingRequest, StreamingResponse> *streamer
) {

    info_log("read_results_loop_thread starting");
    StreamingResponse response;
    std::chrono::steady_clock::time_point first_received_at;
    bool update_first_received_at = true;

    while (streamer->Read(&response)) {
        if (self.is_stopped())
            break;

        // Yandex v3 response has oneof Event:
        //   partial (AlternativeUpdate) — interim
        //   final (AlternativeUpdate) — finalized for current utterance
        //   eou_update — end-of-utterance marker
        //   final_refinement (FinalRefinement.normalized_text) — post-processed final
        //   status_code — status
        //   classifier_update — classifier
        bool is_final = false;
        const speechkit::stt::v3::AlternativeUpdate* update = nullptr;

        if (response.has_partial()) {
            update = &response.partial();
            is_final = false;
        } else if (response.has_final()) {
            update = &response.final();
            is_final = true;
        } else if (response.has_final_refinement() && response.final_refinement().has_normalized_text()) {
            update = &response.final_refinement().normalized_text();
            is_final = true;
        } else {
            // skip other event types (eou_update, classifier_update, status_code)
            continue;
        }

        if (!update || update->alternatives_size() == 0)
            continue;

        const auto& alt = update->alternatives(0);
        info_log("yandex %s text: %s", is_final ? "final" : "partial", alt.text().c_str());

        auto now = std::chrono::steady_clock::now();
        if (update_first_received_at)
            first_received_at = now;

        // stability: partial = ~0.5, final = 1.0
        double stability = is_final ? 1.0 : 0.5;
        CaptionResult cap_result(0, is_final, stability, alt.text(), "", first_received_at, now);
        update_first_received_at = is_final;
        {
            std::lock_guard<recursive_mutex> lock(self.on_caption_cb_handle.mutex);
            if (self.on_caption_cb_handle.callback_fn) {
                self.on_caption_cb_handle.callback_fn(cap_result);
            }
        }
    }
    info_log("read_results_loop_thread done");
    self.stop();
};

#ifdef GRPC_USE_INCLUDED_CERTS

#include "certs/roots.h"

#endif

static void _audio_sender(CaptionStream &self) {
    info_log("=========== _audio_sender (YANDEX) ENTERED, api_key length: %zu ===========", self.settings.api_key.length());

    try {
        auto options = grpc::SslCredentialsOptions();

#ifdef GRPC_USE_INCLUDED_CERTS
        info_log("using embedded certs");
        std::string certs(ROOTS_PEM, ROOTS_PEM + sizeof(ROOTS_PEM) / sizeof(ROOTS_PEM[0]));
        options.pem_root_certs = certs;
#endif
        auto creds = grpc::SslCredentials(options);

        // === WORKAROUND for gRPC DNS bug on Windows ===
        // Resolve hostname via system DNS, then connect by IP directly.
        const char* yandex_host = "stt.api.cloud.yandex.net";
        std::string target_address = std::string(yandex_host) + ":443";
        info_log("resolving %s via system DNS...", yandex_host);

#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = nullptr;
        int dns_ret = getaddrinfo(yandex_host, "443", &hints, &result);
        if (dns_ret == 0 && result) {
            char ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in* sai = (struct sockaddr_in*)result->ai_addr;
            inet_ntop(AF_INET, &(sai->sin_addr), ip_str, INET_ADDRSTRLEN);
            target_address = std::string("ipv4:") + ip_str + ":443";
            info_log("resolved to %s, using direct IP", target_address.c_str());
            freeaddrinfo(result);
        } else {
            info_log("system DNS resolution failed (code %d), falling back to hostname", dns_ret);
        }

        grpc::ChannelArguments channel_args;
        channel_args.SetSslTargetNameOverride(yandex_host);
        auto channel = grpc::CreateCustomChannel(target_address, creds, channel_args);
        std::unique_ptr<Recognizer::Stub> recognizer(Recognizer::NewStub(channel));

        grpc::ClientContext context;
        // Yandex SpeechKit uses Api-Key authorization
        std::string auth_header = std::string("Api-Key ") + self.settings.api_key;
        context.AddMetadata("authorization", auth_header);
        info_log("api key set in authorization header");

        auto streamer = recognizer->RecognizeStreaming(&context);

        info_log("write yandex session options");
        if (write_config(streamer.get(), self.settings)) {
            info_log("write session options done");

            std::thread downstream_thread(&read_results_loop_thread, std::ref(self), streamer.get());

            info_log("starting audio writes");
            write_audio_loop(streamer.get(), self);
            streamer->WritesDone();
            info_log("audio writing finished");

            info_log("waiting for downstream thread finish");
            downstream_thread.join();
            info_log("downstream thread finished!");

            StreamingResponse response;
            while (streamer->Read(&response))
                continue;
        } else {
            info_log("write session options failed");
        }

        auto status = streamer->Finish();
        if (!status.ok()) {
            error_log("yandex grpc stream error: %s", status.error_message().c_str());
        }
    }
    catch (const std::exception &ex) {
        error_log("_audio_sender exception catch %s", ex.what());
    } catch (const std::string &ex) {
        error_log("_audio_sender exception  string %s", ex.c_str());
    } catch (...) {
        error_log("_audio_sender exception any error");
    }
    info_log("_audio_sender done");
}

bool CaptionStream::is_stopped() {
    return stopped;
}

bool CaptionStream::queue_audio_data(const char *audio_data, const uint data_size) {
    if (is_stopped())
        return false;

    string *str = new string(audio_data, data_size);

    if (settings.max_queue_depth) {
        int cleared_cnt = 0;
        while (audio_queue.size_approx() > settings.max_queue_depth) {
            string *item;
            if (audio_queue.try_dequeue(item)) {
                delete item;
                cleared_cnt++;
            }
        }
        if (cleared_cnt)
            info_log("queue too big, dropped %d old items from queue %s", cleared_cnt, session_pair.c_str());
    }

    audio_queue.enqueue(str);
    return true;
}

string *CaptionStream::dequeue_audio_data(const std::int64_t timeout_us) {
    string *ret;
    if (audio_queue.wait_dequeue_timed(ret, timeout_us))
        return ret;

    return nullptr;
}


void CaptionStream::stop() {
    info_log("CaptionStream stop()");
    on_caption_cb_handle.clear();
    stopped = true;

    string *to_unblock_uploader = new string();
    audio_queue.enqueue(to_unblock_uploader);
}


CaptionStream::~CaptionStream() {
    info_log("~CaptionStream deconstructor");
    if (!is_stopped())
        stop();

    int cleared = 0;
    {
        string *item;
        while (audio_queue.try_dequeue(item)) {
            delete item;
            cleared++;
        }
    }
    info_log("~CaptionStream deconstructor, deleted left %d in queue", cleared);
}
