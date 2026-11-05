# Two images from one file. compose.yaml builds both; a plain `docker build .` gives the demo.
#   app   the API and the web app on one port; live features with your own keys (.env)
#   demo  the same image in demo mode: recorded or deterministic answers, no keys

FROM python:3.12-slim-bookworm AS app
ENV PYTHONUNBUFFERED=1 \
    APP_WEB_DIR=/app/web
WORKDIR /app
# System libraries that some wheels link go here (rasterio, for one, needs libexpat1), installed with apt-get.
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app/ app/
COPY web/ web/
RUN useradd --create-home appuser
USER appuser
EXPOSE 8787
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s \
    CMD python -c "import urllib.request as u; u.urlopen('http://127.0.0.1:8787/api/health', timeout=4)"
CMD ["python", "-m", "uvicorn", "app.server:app", "--host", "0.0.0.0", "--port", "8787"]

FROM app AS demo
ENV APP_DEMO=1
