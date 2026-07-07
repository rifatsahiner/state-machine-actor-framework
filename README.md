# state-machine-actor-framework
Divide and conquire your projects with encapsulated [actors](https://en.wikipedia.org/wiki/Actor_model) that are constructed as [hierarchical state machines](https://en.wikipedia.org/wiki/UML_state_machine)  
Unit-test friendly development as an actor being a full enclapsulated ultimate SUT for TDD

## Features
- Dynamic actor creation and related thread management
- Zero-copy event delivery system with thread safe event queue structures
- Timed event delivery
- Direct or publish-subscribe actor communication
- Unit-test utility methods for checking resulted state of actors after event process
- System-wide state monitoring with built-in TUI
- File logging for every actor on its seperate file with rotation
- Built-in CLI utility for event-injection or other test/trigger purposes

<br>

![Framework TUI](images/tui.png)

This framework is exceptionaly good as a project base for AI asisted/generated development;
- Code complete or agents easily understand the state transition pattern of individual tasks for tab-tab-tab programming
- Fully encapsulated actors present small pills of context for AI that prevents it to go astray

<br>

Inspired and based on 2008 book "Practical UML Statecharts in C/C++" by Miro Samek which is intended for embedded systems.   
This implementation of UML state-machine framework, however, is intended for high level server side applications and written in modern C++17

<br>

# Project Dependencies

## Libraries
- pthread
- asio (standalone)
- libdw-dev
- libgpm-dev
- libncurses-dev

## Tools
- CMake >3.15
- GIT
- pkg-config
- libtool
- autoconf-archive

rescue yourself from concurency/multi-threding hell with fully encapsulated actors
rescue yourself from convoluted logics, 50 tier elseif, numerous flags with UML state-machines 
this framework marries two best concepts of programming on OOP ground: the actor-model and hierarchical UML state-machines (which is essentialy OOP for finite state machines)