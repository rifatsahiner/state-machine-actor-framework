# state-machine-framework
A full-fledged UML state-machine framework with support for hierarchical (nested) states   

- Divide and conquire with encapsulated 'tasks' that are constructed as [hierarchical state machines](https://en.wikipedia.org/wiki/UML_state_machine)
- Dynamic task creation and related thread management
- Zero-copy event delivery system with thread safe event queue structures
- Timed event delivery
- Publish-subscribe communication
- System-wide state monitoring with comprehensive TUI

<br>

This framework is exceptionaly good as a project base for AI asisted/generated code;
- Code complate or agents easily understand the state transition pattern of individual tasks for tab-tab-tab programming
- Fully encapsulated tasks present small pills of context for AI that prevents it to go astray

<br>

Inspired and based on 2008 book of "Practical UML Statecharts in C/C++" which is intended for embedded systems.   
This implementation of UML state-machine framework however is intended for high level server side applications and written in modern C++17

<br>

# Project Dependencies

## Libraries
- pthread
- asio (standalone)
- dw
- gpm
- ncurses

## Tools
- GNU CXX 20
- CMake >3.15
- GIT
- pkg-config
- libtool
- automake
- autoconf
- autotools-dev
- autoconf-archive
