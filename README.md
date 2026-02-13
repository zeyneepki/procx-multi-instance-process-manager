# ProcX – Multi-Instance Process Manager (Linux / C)

ProcX is a command-line based process management application developed in C for Linux systems.  
It is designed to support multiple running instances simultaneously while sharing a common process table using inter-process communication (IPC) mechanisms.

When multiple ProcX instances are executed in different terminals, they communicate and synchronize through shared memory, semaphores, and message queues. The system ensures consistent process tracking and coordinated lifecycle management across all active instances.

---

## Project Overview

ProcX allows users to:

- Start new processes using `fork()` and `execvp()`
- Run processes in **attached** or **detached** mode
- Track running processes in a shared process table
- Monitor process lifecycle with a background thread
- Automatically clean up terminated processes
- Broadcast START / TERMINATE events across instances

The application ensures synchronization using named semaphores and maintains shared state using POSIX shared memory.

---

## Architecture & Design

### Multi-Instance Support
All running ProcX instances share:
- A common process table
- Active instance count
- IPC communication channels

Synchronization is handled using named semaphores to prevent race conditions.

### Process Execution
- `fork()` is used to create child processes
- `execvp()` is used to execute external programs
- Detached mode uses `setsid()` and `/dev/null` redirection

### Monitoring
A dedicated monitoring thread periodically:
- Checks running processes
- Detects terminated processes
- Cleans up stale records automatically

### Inter-Process Communication (IPC)
- POSIX Shared Memory (shared process table)
- Named Semaphores (synchronization)
- System V Message Queue (instance event broadcasting)

---

## Technologies Used

- C (Linux)
- pthread
- POSIX Shared Memory
- Named Semaphores
- System V Message Queue
- fork / execvp

---

## Key Features

- True multi-instance synchronization
- Shared process state across terminals
- Attached / Detached execution modes
- Automatic cleanup mechanism
- Safe IPC resource deallocation when the last instance exits

---

## Academic Context

This project was developed as a systems programming assignment focusing on:
- Inter-process communication
- Process lifecycle management
- Concurrency and synchronization
- Linux system calls
