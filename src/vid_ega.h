int     ega_init();
void    ega_out(uint16_t addr, uint8_t val, void *priv);
uint8_t ega_in(uint16_t addr, void *priv);
void    ega_poll();
void    ega_recalctimings();
void    ega_write(uint32_t addr, uint8_t val, void *priv);
uint8_t ega_read(uint32_t addr, void *priv);
