#include <functional>
#include <memory>
#include <thread>
#include <mutex>

#include "temi_action_interfaces/action/temicommand.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "rclcpp_components/register_node_macro.hpp"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <third_party/json.hpp>


namespace temi_action_cpp
{
class SpeakActionServer : public rclcpp::Node
{
public:
  using Temicommand = temi_action_interfaces::action::Temicommand;
  using GoalHandleTemicommand = rclcpp_action::ServerGoalHandle<Temicommand>;
  using json = nlohmann::json;
  typedef websocketpp::server<websocketpp::config::asio> server;

  explicit SpeakActionServer(const rclcpp::NodeOptions & options = rclcpp::NodeOptions())
  : Node("temi_speak_action_server", options)
  {
    using namespace std::placeholders;

    this->action_server_ = rclcpp_action::create_server<Temicommand>(
      this,
      "temi_speak",
      std::bind(&SpeakActionServer::handle_goal, this, _1, _2),
      std::bind(&SpeakActionServer::handle_cancel, this, _1),
      std::bind(&SpeakActionServer::handle_accepted, this, _1));

      try {
          // Set logging settings
          wesocket_server.set_access_channels(websocketpp::log::alevel::all);
          wesocket_server.clear_access_channels(websocketpp::log::alevel::frame_payload);

          // Initialize ASIO
          wesocket_server.init_asio();

          wesocket_server.set_open_handler(bind(&SpeakActionServer::on_connection_open, this, std::placeholders::_1));
          wesocket_server.set_close_handler(bind(&SpeakActionServer::on_connection_close, this, std::placeholders::_1));

          // Register our message handler
          wesocket_server.set_message_handler(std::bind(&SpeakActionServer::on_ws_message, this, &wesocket_server, std::placeholders::_1, std::placeholders::_2));

          // Listen on port 8765
          wesocket_server.listen(8765);

          // Start the server accept loop
          wesocket_server.start_accept();

          // Start the ASIO io_service run loop
          // wesocket_server.run();

          // Launch the server on a separate thread
          std::thread server_thread([this]() {
              wesocket_server.run();
          });

          // Continue with other tasks in the main thread
          server_thread.detach();  // Optionally wait for

      } catch (websocketpp::exception const & e) {
          std::cout << "Exception: " << e.what() << std::endl;
      } catch (...) {
          std::cout << "Unknown exception" << std::endl;
      }
  }

private:
  rclcpp_action::Server<Temicommand>::SharedPtr action_server_;
  server wesocket_server;
  std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> m_connections;
  std::mutex m_connection_lock, m_status_lock;
  json task_status;

  rclcpp_action::GoalResponse handle_goal(
    const rclcpp_action::GoalUUID & uuid,
    std::shared_ptr<const Temicommand::Goal> goal)
  {
    RCLCPP_INFO(this->get_logger(), "Received goal request with command: %d, to say: %s", goal->command, goal->text.c_str());
    (void)uuid;
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }

  rclcpp_action::CancelResponse handle_cancel(
    const std::shared_ptr<GoalHandleTemicommand> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
    (void)goal_handle;
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void handle_accepted(const std::shared_ptr<GoalHandleTemicommand> goal_handle)
  {
    using namespace std::placeholders;
    // this needs to return quickly to avoid blocking the executor, so spin up a new thread
    std::thread{std::bind(&SpeakActionServer::execute, this, _1), goal_handle}.detach();
  }

  void execute(const std::shared_ptr<GoalHandleTemicommand> goal_handle)
  {
    RCLCPP_INFO(this->get_logger(), "Executing Temi Command");
    rclcpp::Rate loop_rate(10);
    const auto goal = goal_handle->get_goal();
    auto feedback = std::make_shared<Temicommand::Feedback>();
    auto result = std::make_shared<Temicommand::Result>();
    bool _task_complete = false;

    task_status = {
      {"command", 1},
      {"status", "pending"},
    };

    json temi_json_msg = {
      {"command", 1},
      {"text", goal->text},
      {"x", goal->x},
      {"y", goal->y},
      {"angle", goal->angle}
    };

    // Send the Required Command to the Temi Robot
    send_json_message(temi_json_msg.dump());

    while(!(_task_complete) && rclcpp::ok())
    {
      // Check if there is a cancel request
      if (goal_handle->is_canceling()) {
        result->completed = false;
        goal_handle->canceled(result);
        RCLCPP_INFO(this->get_logger(), "Goal canceled");
        return;
      }

      RCLCPP_INFO(this->get_logger(), "Received goal request with command: %d, to say: %s", goal->command, goal->text.c_str());


      // Update status
      m_status_lock.lock();
      feedback->status = task_status["status"];
      if (feedback->status == "complete")
      {_task_complete = true;}
      m_status_lock.unlock();


      // Publish feedback
      goal_handle->publish_feedback(feedback);
      RCLCPP_INFO(this->get_logger(), "Publish feedback");

      loop_rate.sleep();
    }

    // Check if goal is done
    if (rclcpp::ok()) {
      result->completed = true;
      goal_handle->succeed(result);
      RCLCPP_INFO(this->get_logger(), "Task Completed");
    }
  }

    // Websocket Related Function ///////////////////////////////////////////////////////////
    void on_ws_message(server* s, websocketpp::connection_hdl hdl, server::message_ptr msg) {
      std::lock_guard<std::mutex> lock(m_status_lock);
      task_status = json::parse(msg->get_payload());
    }

    void on_connection_open(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(m_connection_lock);
        m_connections.insert(hdl);
    }

    void on_connection_close(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(m_connection_lock);
        m_connections.erase(hdl);
    }

    void send_json_message(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_connection_lock);
        for (auto it : m_connections) {
            wesocket_server.send(it, message, websocketpp::frame::opcode::text);
        }
    }

};  // class SpeakActionServer

}  // namespace temi_action_cpp

RCLCPP_COMPONENTS_REGISTER_NODE(temi_action_cpp::SpeakActionServer)