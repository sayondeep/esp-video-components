FROM python:3.11-slim

WORKDIR /app

# Install dependencies
RUN apt-get update && apt-get install -y \
    gcc \
    && rm -rf /var/lib/apt/lists/*

# Copy requirements
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application
COPY server.py .
COPY templates/ ./templates/
COPY static/ ./static/

# Create data and certs directories
RUN mkdir -p /app/data /app/certs

# Expose ports
EXPOSE 1234 8080

# Run server
CMD ["python", "server.py", "--working-directory", "/app/data", "--server-ip", "0.0.0.0", "--host", "0.0.0.0"]
