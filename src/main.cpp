#include "chronolane/core.hpp"
#include "chronolane/ipc.hpp"
#include "chronolane/render.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace chronolane;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
TimeNs monotonic_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) throw std::runtime_error("clock_gettime failed");
    return TimeNs(t.tv_sec) * kSecond + t.tv_nsec;
}
void sleep_until(TimeNs deadline) {
    timespec t{deadline / kSecond, deadline % kSecond};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr) == EINTR && !interrupted) {}
}
const char* configuration(const char* name) {
    if (const char* value = std::getenv(name)) return value;
    const std::string_view key(name);
    if (key.starts_with("CHERS_")) {
        const auto legacy = std::string("CHRONOLANE_") + std::string(key.substr(6));
        return std::getenv(legacy.c_str());
    }
    return nullptr;
}
bool flag(const char* name) { const char* p = configuration(name); return p && std::string_view(p) == "1"; }
std::string setting(const char* name, std::string fallback) {
    const char* p = configuration(name); return p && *p ? p : std::move(fallback);
}
std::string quoted(std::string_view input) {
    std::ostringstream s; s << '"';
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') s << '\\' << c;
        else if (c < 32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else s << c;
    }
    return s.str() + '"';
}
int bind_worker(unsigned from_last) {
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) != 0) return -1;
    std::vector<int> cpus;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &available)) cpus.push_back(i);
    if (cpus.size() < 4 || from_last >= cpus.size()) return -1;
    int cpu = cpus[cpus.size() - 1 - from_last];
    cpu_set_t chosen; CPU_ZERO(&chosen); CPU_SET(cpu, &chosen);
    return pthread_setaffinity_np(pthread_self(), sizeof(chosen), &chosen) == 0 ? cpu : -1;
}
struct Timing {
    std::mutex mutex;
    std::vector<TimeNs> frame_gaps, render_times, polling_gaps;
    int input_cpu = -1, model_cpu = -1;
    Timing() { frame_gaps.reserve(6000); render_times.reserve(6000); polling_gaps.reserve(70000); }
};
struct Distribution {
    std::size_t count{}, over16{}, over2{};
    double mean{}, p50{}, p99{}, maximum{};
};
Distribution distribution(std::vector<TimeNs> values) {
    Distribution d; d.count = values.size(); if (values.empty()) return d;
    long double sum = 0;
    for (auto v : values) { sum += v; d.over16 += v > 16*kMs; d.over2 += v > 2*kMs; }
    std::sort(values.begin(), values.end());
    d.mean = double(sum / values.size() / kMs);
    d.p50 = double(values[(values.size()-1)/2]) / kMs;
    d.p99 = double(values[std::min(values.size()-1, std::size_t(std::ceil(values.size()*0.99)-1))]) / kMs;
    d.maximum = double(values.back()) / kMs;
    return d;
}
void write_distribution(std::ostream& out, const Distribution& d) {
    out << "{\"count\":" << d.count << ",\"mean_ms\":" << d.mean
        << ",\"p50_ms\":" << d.p50 << ",\"p99_ms\":" << d.p99
        << ",\"max_ms\":" << d.maximum << ",\"over_16ms\":" << d.over16
        << ",\"over_2ms\":" << d.over2 << '}';
}
void write_score(std::ostream& out, const ScoreSummary& s) {
    out << "{\"notes\":" << s.notes << ",\"perfect\":" << s.perfect << ",\"good\":" << s.good
        << ",\"empty\":" << s.empty << ",\"missed\":" << s.missed << ",\"raw_score\":" << s.raw_score
        << ",\"score\":";
    if (s.notes) out << s.normalized(); else out << "null";
    out << ",\"delaySum_ms\":" << double(s.delay_sum_ns)/kMs
        << ",\"hit_delay_sum_ms\":" << double(s.hit_delay_sum_ns)/kMs << '}';
}
struct Snapshot {
    RenderState state;
    std::vector<RenderNote> notes;
    std::array<LaneFeedback,16> feedback{};
    Snapshot() { notes.reserve(512); }
};
struct App {
    Engine engine;
    ipc::Server server;
    std::mutex core_mutex, human_mutex;
    std::array<int,4096> human_queue{};
    std::size_t human_count = 0;
    std::atomic<bool> human_overflow{false}, running{true}, finished{false};
    std::atomic<TimeNs> origin{0};
    std::atomic<bool> stopped_early{false};
    Timing timing;
    ScoreSummary final_score{};
    App(Config config, Chart chart, std::string endpoint)
        : engine(config,std::move(chart)), server(std::move(endpoint)) {}

    void enqueue(int command) {
        std::lock_guard lock(human_mutex);
        if (human_count == human_queue.size()) human_overflow = true;
        else human_queue[human_count++] = command;
    }
    void invalidate(std::string reason) {
        std::lock_guard lock(core_mutex);
        if(!finished) engine.invalidate(std::move(reason));
    }
    void start(TimeNs now) { TimeNs expected=0; origin.compare_exchange_strong(expected,now); }
    void tap(std::uint32_t lane, TimeNs now) {
        const auto begin = origin.load();
        if (!begin || finished.load()) return;
        std::lock_guard lock(core_mutex);
        if (lane >= engine.config().lanes) { engine.invalidate("tap outside configured lane range"); return; }
        engine.hit(now-begin,lane);
    }
    void abort(std::string reason) {
        if (origin && !finished) { invalidate(std::move(reason)); stopped_early=true; }
        running=false;
    }
    void snapshot(Snapshot& out, TimeNs absolute_now) {
        const TimeNs begin = origin.load();
        const TimeNs now = begin ? absolute_now-begin : 0;
        out.notes.clear();
        auto& s = out.state;
        s.now_ns=now;
        s.lanes=int(engine.config().lanes);
        s.phase_start_ns=0; s.phase_end_ns=0;
        if (!begin) s.phase=RenderPhase::ready;
        else if (finished.load()) s.phase=RenderPhase::finished;
        else if (now<Timeline::calibration_start) {
            s.phase=RenderPhase::preroll; s.phase_end_ns=Timeline::calibration_start;
        } else if (now<Timeline::calibration_tail_end) {
            s.phase=RenderPhase::calibration; s.phase_start_ns=Timeline::calibration_start; s.phase_end_ns=Timeline::calibration_end;
        } else if (now<Timeline::scored_start) {
            s.phase=RenderPhase::waiting; s.phase_start_ns=Timeline::calibration_tail_end; s.phase_end_ns=Timeline::scored_start;
        } else {
            s.phase=RenderPhase::scored; s.phase_start_ns=Timeline::scored_start; s.phase_end_ns=Timeline::scored_end;
        }
        {
            std::lock_guard lock(core_mutex);
            s.valid=engine.valid();
            if (begin && !finished) {
                const auto& all=engine.notes();
                auto it=std::lower_bound(all.begin(),all.end(),now-kGoodWindow,
                    [](const Note& note,TimeNs t){return note.target_ns<t;});
                for (;it!=all.end() && it->target_ns<=now+kSecond;++it)
                    if (!it->consumed) out.notes.push_back({std::uint8_t(it->lane),it->target_ns});
            }
            const auto& feedback=engine.last_feedback();
            for (std::size_t i=0;i<feedback.size();++i) {
                auto kind=RenderFeedbackKind::none;
                if(feedback[i].kind==Judgment::Perfect) kind=RenderFeedbackKind::perfect;
                else if(feedback[i].kind==Judgment::Good) kind=RenderFeedbackKind::good;
                else if(feedback[i].kind==Judgment::Empty) kind=RenderFeedbackKind::poor;
                out.feedback[i]={feedback[i].event_ns,feedback[i].delay_ns,kind};
            }
            if(finished) {
                const auto& f=final_score;
                s.summary={f.perfect,f.good,f.empty,f.missed,f.notes,f.raw_score,std::uint64_t(f.delay_sum_ns),std::uint64_t(f.hit_delay_sum_ns)};
            }
        }
        s.notes=out.notes; s.feedback=out.feedback;
    }
    void input_loop() {
        try {
            timing.input_cpu=bind_worker(0);
            TimeNs deadline=monotonic_ns(), previous=0;
            std::array<int,4096> commands{};
            while(running) {
                const auto now=monotonic_ns();
                const auto begin=origin.load();
                if(begin && !finished && previous) {
                    std::lock_guard lock(timing.mutex); timing.polling_gaps.push_back(now-previous);
                }
                previous=now;
                server.poll([&](const ipc::Event& event) {
                    switch(event.kind) {
                    case ipc::Kind::Start: start(TimeNs(event.receive_ns)); break;
                    case ipc::Kind::Tap: tap(event.lane,TimeNs(event.receive_ns)); break;
                    case ipc::Kind::Stop: abort("external STOP before completion"); break;
                    case ipc::Kind::Overflow: case ipc::Kind::ProtocolError:
                        invalidate(event.detail); break;
                    case ipc::Kind::Disconnect: break;
                    }
                });
                std::size_t count;
                {
                    std::lock_guard lock(human_mutex);
                    count=human_count;
                    std::copy_n(human_queue.begin(),count,commands.begin()); human_count=0;
                }
                if(human_overflow.exchange(false)) invalidate("human input queue overflow");
                for(std::size_t i=0;i<count;++i) {
                    if(commands[i]==-1) start(monotonic_ns());
                    else tap(std::uint32_t(commands[i]),monotonic_ns());
                }
                const auto active=origin.load();
                if(active && !finished && monotonic_ns()-active>=Timeline::finish) {
                    std::lock_guard lock(core_mutex);
                    final_score=engine.results(Stage::Scored);
                    finished=true;
                }
                deadline+=kMs;
                const auto after=monotonic_ns();
                if(deadline<after) deadline=after; // No accumulating relative-sleep drift.
                sleep_until(deadline);
            }
        } catch(const std::exception& e) { abort(std::string("input worker: ")+e.what()); }
    }
    void model_loop() {
        try {
            timing.model_cpu=bind_worker(1);
            Snapshot state;
            std::vector<std::uint32_t> pixels(kModelWidth*kModelHeight);
            TimeNs deadline=monotonic_ns(),previous=0;
            while(running) {
                const auto before=monotonic_ns();
                snapshot(state,before);
                if(!render_rgba(pixels,kModelWidth,kModelHeight,state.state)) throw std::runtime_error("model renderer rejected state");
                server.publish(reinterpret_cast<const std::uint8_t*>(pixels.data()));
                const auto after=monotonic_ns();
                if(origin && !finished) {
                    std::lock_guard lock(timing.mutex);
                    if(previous) timing.frame_gaps.push_back(after-previous);
                    timing.render_times.push_back(after-before);
                }
                previous=after;
                deadline+=12'500'000;
                if(deadline<after) deadline=after; // Skip obsolete deadlines, never emit a catch-up burst.
                sleep_until(deadline);
            }
        } catch(const std::exception& e) { abort(std::string("model frame worker: ")+e.what()); }
    }
};

fs::path save_results(App& app,const fs::path& base) {
    std::time_t t=std::time(nullptr); std::tm tm{}; localtime_r(&t,&tm);
    std::ostringstream name; name<<std::put_time(&tm,"%Y%m%d-%H%M%S")<<'-'<<getpid();
    fs::path directory=base/name.str(); fs::create_directories(directory);
    fs::permissions(directory,fs::perms::owner_all,fs::perm_options::replace);
    Distribution frames,render,polling;
    {
        std::lock_guard lock(app.timing.mutex);
        frames=distribution(app.timing.frame_gaps); render=distribution(app.timing.render_times);
        polling=distribution(app.timing.polling_gaps);
        std::ofstream samples(directory/"timing_samples.csv");
        samples<<"kind,index,duration_ns\n";
        const auto emit=[&](const char* kind,const std::vector<TimeNs>& values) {
            for(std::size_t i=0;i<values.size();++i) samples<<kind<<','<<i<<','<<values[i]<<'\n';
        };
        emit("frame_gap",app.timing.frame_gaps);emit("render",app.timing.render_times);emit("poll",app.timing.polling_gaps);
        samples.close();if(!samples) throw std::runtime_error("failed writing timing samples");
    }
    std::lock_guard lock(app.core_mutex);
    const auto score=app.engine.results(Stage::Scored),calibration=app.engine.results(Stage::Calibration);
    const auto& chart=app.engine.chart();
    std::ofstream report(directory/"result.json");
    if(!report) throw std::runtime_error("cannot create result.json");
    report<<std::setprecision(12)<<"{\n  \"benchmark\":\"CHERS\",\"version\":\"0.2.0\",\n"
        <<"  \"completed\":"<<(app.finished?"true":"false")<<",\"valid\":"<<(app.engine.valid()&&app.finished?"true":"false")
        <<",\"invalid_reason\":"<<quoted(app.engine.invalid_reason())<<",\n"
        <<"  \"lanes\":"<<app.engine.config().lanes<<",\"density_attempts_per_second\":"<<app.engine.config().density<<",\n"
        <<"  \"entropy_source\":"<<quoted(chart.entropy_source)<<",\n"
        <<"  \"trial\":"; write_score(report,score);
    report<<",\n  \"calibration\":";write_score(report,calibration);
    report<<",\n  \"generation\":{\"attempts\":"<<chart.scored.attempts<<",\"emitted\":"<<chart.scored.emitted
        <<",\"dropped\":"<<chart.scored.dropped<<",\"actual_notes_per_second\":"<<score.notes/50.0<<"},\n"
        <<"  \"observation\":{\"width\":640,\"height\":360,\"format\":\"RGBA8\",\"target_fps\":80,\"target_max_gap_ms\":16,"
        <<"\"timing_compliant\":"<<(frames.count&&frames.over16==0?"true":"false")<<",\"publication_gaps\":";
    write_distribution(report,frames);report<<",\"render_and_publish\":";write_distribution(report,render);report<<"},\n  \"input_poll_gaps\":";
    write_distribution(report,polling);
    utsname system{};uname(&system);
    report<<",\n  \"system\":{\"kernel\":"<<quoted(system.release)<<",\"input_cpu\":"<<app.timing.input_cpu
        <<",\"model_cpu\":"<<app.timing.model_cpu<<",\"human_gui\":"<<(!flag("CHERS_HEADLESS")?"true":"false")
        <<",\"compiler\":"<<quoted(__VERSION__)<<",\"build_type\":"<<quoted(CHERS_BUILD_TYPE)
        <<",\"sanitizers\":"<<(CHERS_SANITIZERS?"true":"false")<<"},\n"
        <<"  \"timeline_ns\":{\"calibration_start\":"<<Timeline::calibration_start<<",\"calibration_end\":"<<Timeline::calibration_end
        <<",\"scored_start\":"<<Timeline::scored_start<<",\"scored_end\":"<<Timeline::scored_end<<",\"finish\":"<<Timeline::finish<<"}\n}\n";
    report.close();if(!report) throw std::runtime_error("failed writing result.json");
    std::ofstream notes(directory/"notes.csv");
    notes<<"id,lane,target_ns,stage\n";
    for(const auto& n:chart.notes) notes<<n.id<<','<<n.lane<<','<<n.target_ns<<','<<stage_name(n.stage)<<'\n';
    std::ofstream events(directory/"inputs.csv");
    events<<"event_ns,lane,note_id,stage,judgment,delay_ns\n";
    for(const auto& e:app.engine.records()) {
        events<<e.event_ns<<','<<e.lane<<',';
        if(e.note_id!=kNoNote) events<<e.note_id;
        events<<','<<stage_name(e.stage)<<','<<judgment_name(e.kind)<<','<<e.delay_ns<<'\n';
    }
    notes.close();events.close();
    if(!notes||!events) throw std::runtime_error("failed writing replay records");
    std::cout<<"RESULT "<<directory.string()<<"\nScore: ";
    if(score.notes) std::cout<<100*score.normalized()<<'%';else std::cout<<"N/A";
    std::cout<<"  delaySum: "<<double(score.delay_sum_ns)/kMs<<" ms  P/G/E/M: "
        <<score.perfect<<'/'<<score.good<<'/'<<score.empty<<'/'<<score.missed
        <<"  frame p99/max: "<<frames.p99<<'/'<<frames.maximum<<" ms\n"<<std::flush;
    return directory;
}
void write_ppm(const fs::path& path,const std::vector<std::uint32_t>& pixels,int w,int h) {
    std::ofstream file(path,std::ios::binary);file<<"P6\n"<<w<<' '<<h<<"\n255\n";
    for(auto p:pixels) { char rgb[3]={char(p),char(p>>8),char(p>>16)};file.write(rgb,3); }
}
Config arguments(int argc,char** argv) {
    if(argc!=3) throw std::invalid_argument("usage: chers LANES DENSITY (lanes 2..16; density 2*lanes..24*lanes)");
    Config c;
    std::string_view lanes(argv[1]);
    auto parsed=std::from_chars(lanes.data(),lanes.data()+lanes.size(),c.lanes);
    if(parsed.ec!=std::errc{}||parsed.ptr!=lanes.data()+lanes.size()) throw std::invalid_argument("invalid integer lane count");
    char* end=nullptr;errno=0;c.density=std::strtod(argv[2],&end);
    if(errno||end==argv[2]||*end) throw std::invalid_argument("invalid note density");
    c.validate();return c;
}
}

int main(int argc,char** argv) {
    if(argc==2&&(std::string_view(argv[1])=="--help"||std::string_view(argv[1])=="--version")) {
        std::cout<<"CHERS 0.2.0\nUsage: chers LANES DENSITY\nLANES 2..16, DENSITY 2*LANES..24*LANES attempts/s\n"
            <<"Space/Enter: start. A-P: tap. Escape: exit. No audio.\nSee README.md and agent.md.\n";return 0;
    }
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* texture=nullptr;
    try {
        const Config config=arguments(argc,argv);
        std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
        HardwareRandom random;
        auto chart=generate_chart(config,random);
        std::cout<<"CHERS: "<<config.lanes<<" lanes, "<<config.density<<" attempts/s; "<<chart.entropy_source<<'\n';
        App app(config,std::move(chart),setting("CHERS_SOCKET",ipc::default_endpoint()));
        std::cout<<"ENDPOINT "<<app.server.endpoint()<<"\nREADY: Space/Enter or client START\n"<<std::flush;
        const bool headless=flag("CHERS_HEADLESS"),exit_after=flag("CHERS_EXIT_AFTER_RUN");
        if(!headless) {
            if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
            window=SDL_CreateWindow("CHERS - visual timing benchmark",1280,720,SDL_WINDOW_RESIZABLE);
            if(!window) throw std::runtime_error(SDL_GetError());
            renderer=SDL_CreateRenderer(window,nullptr);if(!renderer) throw std::runtime_error(SDL_GetError());
            SDL_SetRenderVSync(renderer,0);
            texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,1280,720);
            if(!texture) throw std::runtime_error(SDL_GetError());
            SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_LINEAR);
        }
        std::jthread input,model;
        struct StopGuard { App& app;~StopGuard(){app.running=false;} } guard{app};
        input=std::jthread([&]{app.input_loop();});
        model=std::jthread([&]{app.model_loop();});
        if(flag("CHERS_AUTOSTART")) app.enqueue(-1);
        Snapshot human;
        int human_width=1280,human_height=720;
        std::vector<std::uint32_t> human_pixels(1280*720);
        TimeNs human_deadline=0,finish_seen=0;
        bool saved=false;
        fs::path result_path;
        while(app.running) {
            if(interrupted) {app.abort("interrupted by signal");break;}
            SDL_Event event{};
            while(!headless&&SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT) app.abort("human window closed before completion");
                if(event.type==SDL_EVENT_KEY_DOWN) {
                    if(event.key.scancode==SDL_SCANCODE_ESCAPE) app.abort("Escape before completion");
                    else if(event.key.scancode==SDL_SCANCODE_SPACE||event.key.scancode==SDL_SCANCODE_RETURN) app.enqueue(-1);
                    else if(event.key.scancode>=SDL_SCANCODE_A&&event.key.scancode<SDL_SCANCODE_A+int(config.lanes))
                        app.enqueue(int(event.key.scancode)-SDL_SCANCODE_A);
                }
            }
            const auto now=monotonic_ns();
            if(!headless&&now>=human_deadline) {
                int output_width=0,output_height=0;
                if(!SDL_GetRenderOutputSize(renderer,&output_width,&output_height))
                    throw std::runtime_error(SDL_GetError());
                if(output_width>0&&output_height>0&&
                   (output_width!=human_width||output_height!=human_height)) {
                    auto* resized=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING,output_width,output_height);
                    if(!resized) throw std::runtime_error(SDL_GetError());
                    SDL_DestroyTexture(texture);texture=resized;
                    human_width=output_width;human_height=output_height;
                    human_pixels.resize(std::size_t(human_width)*human_height);
                }
                app.snapshot(human,now);
                void* data=nullptr;int pitch=0;
                if(!SDL_LockTexture(texture,nullptr,&data,&pitch)) throw std::runtime_error(SDL_GetError());
                if(pitch==human_width*4) {
                    if(!render_rgba({static_cast<std::uint32_t*>(data),std::size_t(human_width)*human_height},human_width,human_height,human.state))
                        throw std::runtime_error("human renderer rejected state");
                } else {
                    if(!render_rgba(human_pixels,human_width,human_height,human.state))
                        throw std::runtime_error("human renderer rejected state");
                    for(int y=0;y<human_height;++y) std::memcpy(static_cast<char*>(data)+y*pitch,human_pixels.data()+y*human_width,human_width*4);
                }
                SDL_UnlockTexture(texture);
                SDL_RenderClear(renderer);SDL_RenderTexture(renderer,texture,nullptr,nullptr);SDL_RenderPresent(renderer);
                human_deadline=now+8'333'333;
            }
            if(app.finished&&!saved) {
                result_path=save_results(app,setting("CHERS_OUTPUT","runs"));saved=true;finish_seen=now;
                Snapshot final;app.snapshot(final,now);
                std::vector<std::uint32_t> pixels(kModelWidth*kModelHeight);
                (void)render_rgba(pixels,kModelWidth,kModelHeight,final.state);
                write_ppm(result_path/"final.ppm",pixels,kModelWidth,kModelHeight);
            }
            if(saved&&exit_after&&now-finish_seen>500*kMs) app.running=false;
            sleep_until(monotonic_ns()+kMs);
        }
        app.running=false;
        input.join();model.join();
        if(app.origin&&!saved) {
            if(app.engine.valid()) app.invalidate("run did not complete");
            result_path=save_results(app,setting("CHERS_OUTPUT","runs"));
        }
        SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
        return app.engine.valid()&&app.finished?0:2;
    } catch(const std::exception& e) {
        std::cerr<<"CHERS error: "<<e.what()<<'\n';
        SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
}
