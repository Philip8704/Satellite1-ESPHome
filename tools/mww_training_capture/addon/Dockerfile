ARG BUILD_FROM
FROM $BUILD_FROM

# HA add-on base images are Alpine + s6-overlay + bashio. The system Python
# there is intentionally minimal, so we install our deps via pip into a
# venv to avoid Alpine's PEP 668 "externally-managed" complaint and to keep
# package versions pinned. The venv lives at /opt/mww/venv and run.sh
# activates it before launching the service.
RUN apk add --no-cache python3 py3-pip \
 && python3 -m venv /opt/mww/venv \
 && /opt/mww/venv/bin/pip install --no-cache-dir --upgrade pip \
 && /opt/mww/venv/bin/pip install --no-cache-dir \
        "fastapi>=0.110" \
        "uvicorn[standard]>=0.27" \
        "pydantic>=2.5"

WORKDIR /opt/mww
COPY service /opt/mww/service
COPY run.sh /run.sh
RUN chmod +x /run.sh

CMD [ "/run.sh" ]
