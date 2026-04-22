# syntax=docker/dockerfile:1.6
FROM ubuntu:22.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# ---- Base system packages (pinned) ----
# Pin exact versions for reproducibility
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        wget \
        git \
        build-essential \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        nano \
        htop \
        iproute2 \
        net-tools \
        openssh-client && \
    rm -rf /var/lib/apt/lists/*

# ---- Python environment (optional but common) ----
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Example: install pinned Python deps
# COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir --upgrade pip==24.0 #&& \
#    pip install --no-cache-dir -r /tmp/requirements.txt

# ---- Non-root user (safer default) ----
RUN useradd -m -u 1000 appuser
USER appuser
WORKDIR /home/appuser

# ---- Default command ----
CMD ["bash"]