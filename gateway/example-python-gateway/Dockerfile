FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY hermes_gateway.py .

# Configure via env / --env-file (see .env.example). Non-root.
RUN useradd -m hermes
USER hermes
EXPOSE 8787
CMD ["python", "hermes_gateway.py"]
