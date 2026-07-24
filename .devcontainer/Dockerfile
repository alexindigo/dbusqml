FROM archlinux:base-devel

RUN pacman -Sy --noconfirm --needed \
    qt6-base qt6-declarative \
    cmake ninja clang \
    quickshell \
    dbus \
    shellcheck shfmt \
  && pacman -Scc --noconfirm

RUN useradd -m -s /bin/bash dev
USER dev
WORKDIR /workspaces/dbusqml
