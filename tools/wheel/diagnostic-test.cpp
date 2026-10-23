// Virtual attachment regression. Modified 2026-10-17. GPL-3.0-only.
// File input only; never creates windows or sends physical device output.
#include "diagnostic_input.h"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstdio>
namespace {
uint64_t Now(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());}
void Require(bool value){if(!value)throw std::runtime_error("Diagnostic attachment regression failed");}
}
int main() try {
  const auto file=std::filesystem::temp_directory_path()/(
      "fm4-fixture-"+std::to_string(Now())+".txt");
  struct Cleanup {std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}} cleanup{file};
  _putenv_s("FM4_BACKGROUND","1");
  _putenv_s("FM4_WHEEL_DIAGNOSTIC_INPUT",file.string().c_str());
  _putenv_s("FM4_WHEEL_DIAGNOSTIC_DEVICES","1");
  bool wheel=false;uint8_t devices=0;
  auto read=[&]{std::this_thread::sleep_for(std::chrono::milliseconds(12));
    return fm4::wheel_diagnostic::ReadState(false,&wheel,&devices);};
  auto write=[&](unsigned id,const std::string& fields,unsigned ttl=500){
    std::ofstream output(file);output<<Now()+ttl<<' '<<id<<' '<<fields<<'\n';};
  Require(fm4::wheel_diagnostic::Enabled());
  read();Require(devices==1&&!wheel);  // No wheel attached at startup.
  write(1,"4096 0 0 0 0 0 0");  // Legacy nine-field controller record.
  Require(uint16_t(read().gamepad.buttons)==4096&&devices==1&&!wheel);
  write(2,"0 0 128 0 0 0 0 1 3");
  Require(read().gamepad.right_trigger==128&&wheel&&devices==3);
  write(3,"0 0 0 0 0 0 0 0 1",35);read();
  std::this_thread::sleep_for(std::chrono::milliseconds(45));
  Require(read().gamepad.right_trigger==0&&devices==1); // Unplug stays unplugged after TTL.
  write(4,"4096 0 0 0 0 0 0 1 1");read();
  Require(devices==1&&!wheel); // Reject wheel input while the wheel is absent.
  write(5,"0 0 0 0 0 0 0 0 0");read();Require(devices==0);
  write(6,"0 0 0 0 0 0 0 1 2");read();Require(devices==2&&wheel);
  write(7,"0 0 0 0 0 0 0 0 4");read();Require(devices==2); // Invalid mask.
  write(8,"0 0 0 0 0 0 0 bad");read();Require(devices==2); // Invalid mode.
  write(9,"0 0 0 0 0 0 0 0 1");read();Require(devices==1&&!wheel);
  std::puts("Controller-only startup, idle attachment, expiry, reconnect and invalid-record checks pass.");
  return 0;
} catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
