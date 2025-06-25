# Build:
# docker build -o . .

# Use an official image
FROM devkitpro/devkitppc:20250527 AS usbloadergx
 
# Copy current folder into container, then compile
COPY . /projectroot/
RUN cd /projectroot && make zip -j$(nproc)

# Copy the ZIP file out of the container
FROM scratch AS export-stage
COPY --from=usbloadergx /projectroot/usbloader_gx.zip /
