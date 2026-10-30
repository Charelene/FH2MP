# The one test environment, used by GitHub (.github/workflows/ci.yml) and on a developer machine (release/ci.sh --container), so a
# check that passes in one passes in the other. Python 3.12 as in the release jobs, Debian's Wine and mingw-w64 for the shim checks,
# PostgreSQL 16 as on the servers, and the hashed test and launcher requirements. Bump the base image or a package on purpose.
FROM docker.io/library/python:3.12.14-slim-bookworm@sha256:392307d22300de8b5986851a12d9176dfc0fc073e65bf6523ebd7dcbeb23564e

ARG PGDG_FINGERPRINT=B97B0AFCAA1A47F044F244A07FCC7D46ACCC4CF8
RUN set -eux; \
    apt-get -o Acquire::Retries=3 update; \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg; \
    curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc -o /tmp/pgdg.asc; \
    gpg --batch --show-keys --with-colons /tmp/pgdg.asc | grep -q "^fpr:::::::::${PGDG_FINGERPRINT}:"; \
    gpg --batch --dearmor -o /usr/share/keyrings/pgdg.gpg /tmp/pgdg.asc; \
    echo "deb [signed-by=/usr/share/keyrings/pgdg.gpg] https://apt.postgresql.org/pub/repos/apt bookworm-pgdg main" > /etc/apt/sources.list.d/pgdg.list; \
    apt-get -o Acquire::Retries=3 update; \
    apt-get install -y --no-install-recommends \
        postgresql-16 \
        git gcc libc6-dev wine wine64 gcc-mingw-w64-x86-64 \
        libegl1 libgl1 libfontconfig1 libdbus-1-3 libglib2.0-0 libxkbcommon0 libxkbcommon-x11-0 \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-shape0; \
    rm -rf /var/lib/apt/lists/* /tmp/pgdg.asc

COPY release/requirements-sign.txt release/requirements-test.txt /reqs/release/
COPY launcher/requirements-release.txt /reqs/launcher/
RUN pip install --no-cache-dir --require-hashes -r /reqs/release/requirements-test.txt \
 && pip install --no-cache-dir --require-hashes -r /reqs/launcher/requirements-release.txt

# PostgreSQL refuses to run as root; the checks run as this user.
RUN useradd --create-home --uid 1000 ci
USER ci
ENV PATH=/usr/lib/postgresql/16/bin:$PATH PYTHONDONTWRITEBYTECODE=1 QT_QPA_PLATFORM=offscreen FH2MP_NO_KEYRING=1
WORKDIR /src
