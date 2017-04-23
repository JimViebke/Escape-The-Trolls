#include "client.h"
#include "constants.h"

#include <thread>
#include <iostream>
#include <sstream>
#include <iterator>

#include "wtypes.h"

// Get the horizontal and vertical screen sizes in pixel
void GetDesktopResolution(int& horizontal, int& vertical)
{
	RECT desktop;
	// Get a handle to the desktop window
	const HWND hDesktop = GetDesktopWindow();
	// Get the size of screen to the variable desktop
	GetWindowRect(hDesktop, &desktop);
	// The top left corner will have coordinates (0,0)
	// and the bottom right corner will have coordinates
	// (horizontal, vertical)
	horizontal = desktop.right;
	vertical = desktop.bottom;
}

Client::Client(const unsigned &height, const unsigned &width, std::unique_ptr<pipedat::Connection> &con)
{
	int horizontal = 0;
	int vertical = 0;
	GetDesktopResolution(horizontal, vertical);

	// configure the console
	Console_Framework::setup(height, width, "Escape The Trolls");

	// draw the blue box
	Console_Framework::draw_box(height - 3, 0, 3, width, Constants::UI_BACKGROUND);

	// draw the prompt
	Console_Framework::draw_char(height - 2, 1, '>', Constants::TEXT_COLOR);

	// create our TUI elements
	text_box = std::make_unique<Text_Box>(height - 2, 3, width - 4, Constants::UI_TEXT_COLOR);
	display = std::make_unique<Scrollable_Text_Display>(1, 1, height - 4, width - 2);

	// start the cursor in the textbox
	Console_Framework::set_cursor_visibility(true);
	Console_Framework::set_cursor_position(height - 2, 3);

	connection = std::move(con);
}

void Client::run()
{
	// start the receiving thread here
	std::thread receive(&Client::receive, this);

	for (;;)
	{
		// get the list of events that have occurred
		const Console_Framework::event_list events = Console_Framework::get_events();

		// for each event
		for (const Console_Framework::event_ptr & event : events)
		{
			// if the event is a done event type
			if (const Console_Framework::done_event_ptr done_event = Console_Framework::convert_to<Console_Framework::Done_Event>(event))
			{
				connection->shut_down();
				receive.join();
				return; // somehow kill the other thread (or just let it die)
			}
			else if (const Console_Framework::scroll_event_ptr scroll_event = Console_Framework::convert_to<Console_Framework::Scroll_Event>(event))
			{
				// scroll and re-render the viewer
				std::lock_guard<std::mutex> lock(display_mutex);
				this->display->scroll(scroll_event->get_direction());
			}
			else if (const Console_Framework::key_event_ptr key_event = Console_Framework::convert_to<Console_Framework::Key_Event>(event))
			{
				// if the enter key was pressed
				if (key_event->enter_pressed())
				{
					// read the message
					const std::string message = text_box->take_contents();

					// add the message to the sender's screen (better than having the server send it back)		
					std::lock_guard<std::mutex> lock(display_mutex);

					std::stringstream ss(message);
					const std::istream_iterator<std::string> begin(ss);
					std::vector<std::string> strings(begin, std::istream_iterator<std::string>());

					if (strings.size() > 1 && strings[0] == "/name")
						user_name = message.substr(6, message.size());

					// if the user entered a command, don't render it (let the server send the results)
					if (message.size() > 0 && message[0] == '/')
					{
						// ...unless the command was a non-empty whisper
						if (strings.size() >= 3 && (strings[0] == "/w" || strings[0] == "/whisper"))
						{
							std::stringstream ss;
							// substring, skipping over the first two words, plus two spaces
							ss << "You whispered to " << strings[1] << ": " << message.substr(strings[0].size() + strings[1].size() + 2);
							display->add(ss.str(), C::TEXT_WHISPER);
						}
					}
					else // no command was entered, render the message
					{
						display->add(user_name + ": " + message, C::TEXT_DEFAULT);
					}

					if (message == "/exit" || message == "/e")
					{
						connection->send("");
						return;
					}

					// send the message
					connection->send(message);
				}
				else // all other key events
				{
					// append the char to the selected textbox's contents and re-render
					text_box->add_char(key_event->get_char());
				}
			}
		}
	}
}

void Client::receive()
{
	try
	{
		for (;;)
		{
			std::string message = connection->receive();

			Console_Framework::color_type color = C::TEXT_DEFAULT;

			if (message.size() >= C::HELP_FLAG.size() && message.substr(0, C::HELP_FLAG.size()) == C::HELP_FLAG)
			{
				message = message.substr(C::HELP_FLAG.size());
				color = C::TEXT_INFO;
			}
			else if (message.size() >= C::WHISPER_FLAG.size() && message.substr(0, C::WHISPER_FLAG.size()) == C::WHISPER_FLAG)
			{
				message = message.substr(C::WHISPER_FLAG.size());
				color = C::TEXT_WHISPER;
			}
			else if (message.size() >= C::INFO_FLAG.size() && message.substr(0, C::INFO_FLAG.size()) == C::INFO_FLAG)
			{
				message = message.substr(C::INFO_FLAG.size());
				color = C::TEXT_INFO;
			}

			std::lock_guard<std::mutex> lock(display_mutex);

			if (message.find(C::HELP_FLAG) != std::string::npos)
			{
				size_t index = 0;
				std::string line;
				while ((index = message.find(C::HELP_FLAG)) != std::string::npos)
				{
					line = message.substr(0, index);
					display->add(line, C::TEXT_INFO);
					message.erase(0, index + C::HELP_FLAG.size());
				}
			}
			else
			{
				display->add(message, color);
			}
		}
	}
	catch (pipedat::disgraceful_disconnect_exception)
	{
		display->add("Server was shutdown by the host", C::TEXT_DEFAULT);
	}
	catch (std::exception)
	{
		display->add("Server shut down unexpectedly.", C::TEXT_DEFAULT);
	}
}
