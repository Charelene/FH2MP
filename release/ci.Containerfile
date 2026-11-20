# The one test environment, used by GitHub (.github/workflows/ci.yml) and on a developer machine (release/ci.sh --container), so a
# check that passes in one passes in the other. Debian's Wine and mingw-w64 for the shim checks, PostgreSQL 16 as on the servers and
# ImageMagick as on them (the Go tests), Go of native/go.mod. The base image is Python's because five shim checks still compute their
# expected values with python3 (test_sha256, test_inflate, test_gamedb_keep, test_gamedb_files, test_diag); no pip package is installed (the
# launcher's and the manifest signer's requirements went with them in 1.5.0). Bump the base image or a package on purpose.
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
        git gcc libc6-dev wine wine64 gcc-mingw-w64-x86-64 imagemagick; \
    rm -rf /var/lib/apt/lists/* /tmp/pgdg.asc

# Go for the port in native/ (release/ci.sh go): the version of native/go.mod, checked against the hash go.dev publishes for the tarball.
ARG GO_VERSION=1.27.3
ARG GO_SHA256=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40
RUN set -eux;     curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" -o /tmp/go.tgz;     echo "${GO_SHA256}  /tmp/go.tgz" | sha256sum -c -;     tar -C /usr/local -xzf /tmp/go.tgz;     rm /tmp/go.tgz

# PostgreSQL refuses to run as root; the checks run as this user.
RUN useradd --create-home --uid 1000 ci
USER ci
ENV PATH=/usr/local/go/bin:/usr/lib/postgresql/16/bin:$PATH PYTHONDONTWRITEBYTECODE=1
WORKDIR /src
