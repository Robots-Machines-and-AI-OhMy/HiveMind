# HiveMind

HiveMind is a distributed computing and resource-sharing platform designed to combine multiple systems into a unified compute environment.

---

# Prerequisites

Before building HiveMind, install the following tools and dependencies.

## 1. Install PowerShell 7

Install via command:

```
iex "& { $(irm https://aka.ms/install-powershell.ps1) } -UseMSI"
```

---

## 2. Install CMake

Download CMake from:

https://cmake.org/

Ensure it is added to your system PATH during installation.

---

## 3. Install Strawberry Perl

Download and install:

https://strawberryperl.com/

Ensure Perl is added to PATH.

---

## 4. Install Visual Studio Build Tools

1. Go to:
https://visualstudio.microsoft.com/downloads/?q=build+tools

2. Expand:
- Tools for Visual Studio

3. Download:
- Build Tools for Visual Studio 2026

4. Run:
- Run the downloaded file

5. Install tools:
- Click on "Desktop development with C++"
- Click install
- Close the installer once done downloading

Additional configuration steps will be added later.

---

# Clone Repository

```
git clone https://github.com/Robots-Machines-and-AI-OhMy/HiveMind.git --recurse-submodules
```

---

# Build Instructions

Run the following in PowerShell:
```
cd HiveMind
./build.bat
```

---

# Notes

- Ensure all dependencies are installed and available in PATH.
- Some components may require administrator privileges.
- If build fails:
  - Verify CMake installation
  - Verify Visual Studio Build Tools installation
  - Verify Perl installation
  - Ensure submodules were cloned correctly

---

# Repository

https://github.com/Robots-Machines-and-AI-OhMy/HiveMind
