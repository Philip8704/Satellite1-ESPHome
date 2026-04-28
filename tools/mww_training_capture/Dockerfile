FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY service /app/service
COPY __init__.py /app/__init__.py

# /data is the on-disk capture root; mount a host volume here so the dataset
# survives container restarts.
ENV MWW_CAPTURE_DATA_DIR=/data/mww_captures
ENV MWW_CAPTURE_HOST=0.0.0.0
ENV MWW_CAPTURE_PORT=8765

EXPOSE 8765
VOLUME ["/data"]

CMD ["python", "-m", "service"]
