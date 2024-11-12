#include "DTCControl.h"
#include "HelixControl.h"
#include "MicrohardControl.h"
#include "PoplarControl.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <unistd.h>
// #include "AriusControl.h"
#include "argparse.h"

bool keep_running = true;

void signalCallbackHandler(int signum) {
  std::cout << "Caught signal " << signum << std::endl;
  keep_running = false;
}

int main(int argc, const char *argv[]) {
  argparse::ArgumentParser parser(
      "radio_control",
      "Radio control binary, utilizes vantage radio control library.");
  // Add host option (set to 20.4)
  parser.add_argument()
      .names({"-g", "--gcs"})
      .description("is host")
      .required(false);

  parser.add_argument()
      .names({"-p", "--power"})
      .description("power level in dBm")
      .required(false);

  parser.add_argument("-i", "--networkid", "networkid", false)
      .description("network id");

  parser.add_argument("-k", "--key", "key", false)
      .description("network encryption key");

  parser.add_argument()
      .names({"-f", "--frequency"})
      .description("channel center frequency")
      .required(false);

  parser.add_argument("-b", "--bandwidth", "bandwidth", false)
      .description("channel bandwidth");

  parser.add_argument("-0", "--bwimmediate", "bwimmediate", false)
      .description("channel bw, change now");

  parser.add_argument("-m", "--model", "model", false)
      .description("Radio type. Options: helix, MH, sbs")
      .required(true);

  parser.add_argument("-a", "--adapter", "adapter", false)
      .description("Network adapter name.");

  parser.add_argument()
      .names({"-t", "--test"})
      .description("Enable RF Test Mode.")
      .required(false);

  parser.add_argument("-l", "--localip", "localip", false)
      .description("Known IP address of the local radio.");

  parser.enable_help();
  auto err = parser.parse(argc, argv);
  if (err) {
    std::cout << err << std::endl;
    return -1;
  }

  if (parser.exists("help")) {
    parser.print_help();
    return 0;
  }

  signal(SIGINT, signalCallbackHandler);

  // TODO: generic radio_control class should have a radio detection method so
  // that we don't have to specify the radio type
  std::shared_ptr<radio_control::RadioControl> radio;
  bool apply_settings = false;

  // TODO: Make the model detection completely automatic.
  if (parser.exists("model")) {
    if (parser.get<std::string>("model") == "helix") {
      if (parser.exists("gcs")) {
        radio = std::make_shared<radio_control::HelixControl>(true);
      } else {
        radio = std::make_shared<radio_control::HelixControl>(false);
      }
    } else if (parser.get<std::string>("model") == "MH") {
      // If not specified it will default to trying to figure things out on its
      // own.
      std::string local_ip = "";
      std::string adapter = "";
      if (parser.exists("localip")) {
        local_ip = parser.get<std::string>("localip");
      }

      if (parser.exists("adapter")) {
        adapter = parser.get<std::string>("adapter");
      }

      if (parser.exists("gcs")) {
        radio = std::make_shared<radio_control::MicrohardControl>(
            true, adapter, "192.168.20.104", local_ip);
      } else {
        radio = std::make_shared<radio_control::MicrohardControl>(
            false, adapter, "192.168.20.105", local_ip);
      }
    } else if (parser.get<std::string>("model") == "sbs") {
      if (parser.exists("gcs")) {
        radio = std::make_shared<radio_control::VRSBSControl>(true);
      } else {
        radio = std::make_shared<radio_control::VRSBSControl>(false);
      }
    } else if (parser.get<std::string>("model") == "dtc") {
      // If not specified it will default to trying to figure things out on its
      // own.
      std::string local_ip = "";
      std::string adapter = "";
      if (parser.exists("localip")) {
        local_ip = parser.get<std::string>("localip");
      } else {
        if (parser.exists("gcs")) {
          local_ip = "192.168.20.4";
        } else {
          local_ip = "192.168.20.30";
        }
      }

      if (parser.exists("adapter")) {
        adapter = parser.get<std::string>("adapter");
      }

      if (parser.exists("gcs")) {
        radio = std::make_shared<radio_control::DTCControl>(
            true, adapter, "192.168.20.104", local_ip);
      } else {
        radio = std::make_shared<radio_control::DTCControl>(
            false, adapter, "192.168.20.105", local_ip);
      }

    } else {
      std::cout << "Invalid model!" << std::endl;
      exit(1);
    }
    // } else if (parser.get<std::string>("model") == "arius") {
    //   if (parser.exists("gcs")) {
    //     radio = std::make_shared<radio_control::AriusControl>(true);
    //   } else {
    //     radio = std::make_shared<radio_control::AriusControl>(false);
    //   }
    // }
  }

  // Wait for the system to be configured to the point where we can send it
  // commands. We queue the commands and only actually parse
  // them in the library when the system is ready for that to occur, though we
  // should really add some sort of "action complete" callback.
  while ((radio->GetModel() == radio_control::RadioModel::UNKNOWN) &&
         keep_running) {
    std::cout << "Waiting for radio to be detected / configurable."
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  // TODO: Make sure we can issue requests/changes at any time without causing
  // problems. Right now with the fddl1624 we need a small pause after detection
  // for it to get to the "radio booted" state and then issue the
  // configurations.
  //
  if (parser.get<std::string>("model") != "sbs") {
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }

  if (parser.exists("test") && parser.exists("power") && parser.exists("frequency")) {
    std::shared_ptr<radio_control::VRSBSControl> vrsbs =
        std::dynamic_pointer_cast<radio_control::VRSBSControl>(radio);
    int power = parser.get<int>("power");
    int frequency = parser.get<int>("frequency");
    int active = parser.get<int>("test");
    std::cout << "ACTIVE: " << active << std::endl;
    vrsbs->EnableRFTestMode(active, power, frequency);
    radio->ApplySettings();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // Immediately return
    return 0;
  }

  if (parser.exists("bwimmediate")) {
    std::shared_ptr<radio_control::VRSBSControl> vrsbs =
        std::dynamic_pointer_cast<radio_control::VRSBSControl>(radio);
    vrsbs->SetRateImmediate(parser.get<int>("bwimmediate"));
  }

  if (parser.exists("power")) {
    radio->SetOutputPower(parser.get<int>("power"));
  }

  if (parser.exists("networkid")) {
    radio->SetNetworkID(parser.get<std::string>("networkid"));
    apply_settings = true;
  }

  if (parser.exists("key")) {
    radio->SetNetworkPassword(parser.get<std::string>("key"));
    apply_settings = true;
  }

  // TODO: Perform this check before we actually instantiate the radio control
  // class.
  if (parser.exists("frequency") && parser.exists("bandwidth")) {
    radio->SetFrequencyAndBW(parser.get<int>("frequency"),
                             parser.get<float>("bandwidth"));
    apply_settings = true;
  } else if (parser.exists("frequency")) {
    std::cout << "Channel bandwidth and frequncy must both be specified if you "
                 "wish to set either."
              << std::endl;
    return -1;
  } else if (parser.exists("bandwidth")) {
    std::cout << "Channel bandwidth and frequncy must both be specified if you "
                 "wish to set either."
              << std::endl;
    return -1;
  }

  if (apply_settings) {
    std::cout << "Committing NV settings." << std::endl;
    radio->ApplySettings();
  }

  while (keep_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return 0;
}
