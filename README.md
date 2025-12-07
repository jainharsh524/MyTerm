# MyTerm – A Custom Terminal with X11 GUI

###  Computing Lab (CS69201)
**Project by:** Harsh Jain  
**Operating System:** macOS  
**Language:** C (POSIX + X11 + pthread)

---

## Overview

**MyTerm** is a custom-built graphical terminal emulator developed in C using the **X11** graphics library.  
It mimics a modern Unix shell with features such as multiple tabs, command execution, I/O redirection, pipes, background jobs, command history, and auto-completion — all displayed inside a standalone GUI window.

The project was implemented and tested on **macOS** using **XQuartz** for X11 display.

---

## System Requirements

| Requirement | Description |
|--------------|-------------|
| **OS** | macOS (tested on macOS 14+) |
| **Display Server** | [XQuartz](https://www.xquartz.org/) |
| **Compiler** | GCC (`brew install gcc`) |
| **Libraries** | X11 Development Libraries (`brew install xorg`) |
| **Threads** | POSIX `pthread` support |

---

## Installation & Compilation

### 1. Clone or copy the project
```bash
git clone https://github.com/yourusername/MyTerm.git
cd MyTerm
````

### 2. Compile the source code

Make sure XQuartz is installed and its headers are accessible.

```bash
gcc myterm.c -o myterm -lX11 -lpthread -lutil
```

### 3. Start XQuartz

Launch **XQuartz** manually or by running:

```bash
open -a XQuartz
```

### 4. Run MyTerm

```bash
./myterm
```

This opens a new **X11 GUI window** for the MyTerm terminal.

---

## Key Features

### 1. Graphical User Interface (X11)

* Built using `Xlib` functions: `XOpenDisplay`, `XCreateSimpleWindow`, `XMapWindow`, and `XDrawString`.
* Each **tab** is a separate shell instance.
* Supports **keyboard and mouse input** inside the GUI.

---

### 2. Execute External Commands

* Runs programs using:

  ```c
  fork();
  execvp(argv[0], argv);
  waitpid(pid, &status, 0);
  ```
* Displays command output inside the GUI window.

---

### 3. Multiline Unicode Input

* Supports Unicode characters using `setlocale(LC_CTYPE, "")`.
* Multi-line input using the `\` continuation character.

---

### 4. Input / Output Redirection

* Redirect input/output using `<`, `>`, and `>>` symbols:

  ```bash
  ./a.out < input.txt
  ls > out.txt
  ls >> append.txt
  ```

---

### 5. Pipe Support

* Implements Unix pipelines using `pipe()` and multiple `fork()` calls:

  ```bash
  ls *.txt | grep log | wc -l
  ```

---

###  6. MultiWatch Command

Custom command to execute multiple commands concurrently:

```bash
multiWatch ["date", "uptime", "who"]
```

* Each command runs in parallel using threads.
* Output labeled with timestamps and command names.

---

### 7. Line Navigation Shortcuts

* **Ctrl+A** → Move cursor to line start
* **Ctrl+E** → Move cursor to line end

---

### 8. Signal Handling

* **Ctrl+C** → Sends `SIGINT` to the foreground process.
* **Ctrl+Z** → Sends `SIGTSTP` and moves job to background.
* Built-in commands:

  * `jobs` → list running background jobs
  * `fg <pid>` → bring job to foreground
  * `kill <pid>` → terminate job

---

### 9. Persistent Command History

* Stores up to **10,000 commands** in `~/.myterm_history`.
* `history` → lists the last 1000 commands.
* **Ctrl+R** → search through command history interactively.

---

### 10. Auto-Completion (Tab)

* Press **Tab** to auto-complete file names in the current directory.
* Shows multiple match suggestions if applicable.

---

### 11. Background Jobs

* Commands ending with `&` run in the background.
* Non-blocking I/O ensures GUI remains responsive while jobs output data asynchronously.

---

### 12. Multi-Tab Interface

* Tabs are independent terminals with their own buffers, history, and jobs.
* Click the **“+”** button to create a new tab.
* Click the **“x”** on a tab to close it.

---

## Example Commands

Try these inside MyTerm:

```bash
ls -l
cat file.txt | grep word | wc -l
multiWatch ["date", "uptime"]
echo "Hello World" > output.txt
sleep 10 &
jobs
fg <pid>
history
```

---

##  Project Structure

```
MyTerm/
│
├── myterm.c              # Main source file (X11 GUI + Shell logic)
├── README.md             # Project documentation
├── MyTerm_Report.tex     # LaTeX report (project description)
├── MyTerm_DesignDoc.tex  # LaTeX design document (with code snippets)
└── .myterm_history       # Auto-generated persistent command history
```

---

##  Internals and Architecture

* **X11 Event Loop:** Handles GUI events (`KeyPress`, `ButtonPress`, etc.)
* **Process Handling:** `fork()` + `execvp()` for command execution
* **Signal Management:** `SIGINT`, `SIGTSTP` for job control
* **Non-blocking I/O:** `fcntl(fd, F_SETFL, O_NONBLOCK)`
* **Threading:** `pthread_create()` used in `multiWatch`
* **Persistent Data:** History stored in `~/.myterm_history`

---

##  Cleanup and Exit

* All background processes are killed before exit.
* Memory from buffers and histories is freed properly.

---

##  Conclusion

**MyTerm** successfully integrates low-level system calls, GUI rendering, and process management to emulate a modern Unix shell experience in a graphical environment.

---

###  Author

**Harsh Jain**
Computing Lab (CS69201)
Department of Computer Science
macOS – XQuartz Edition