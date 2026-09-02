/* webrtcsig.c */
#include <re.h>
#include <baresip.h>
#include "demo.h"

static int module_init(void)
{
	static char cert[256] = "/etc/demo.pem";
	static char www[256]  = "www";
	static char ice[256]  = "";

	struct conf *conf = conf_cur();

	conf_get_str(conf, "webrtcsig_cert", cert, sizeof(cert));
	conf_get_str(conf, "webrtcsig_www", www, sizeof(www));
	conf_get_str(conf, "webrtcsig_ice_server", ice, sizeof(ice));

	return demo_init(cert, www, str_isset(ice) ? ice : NULL,
			  NULL, NULL);
}

static int module_close(void)
{
	demo_close();
	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(webrtcsig) = {
	"webrtcsig",
	"application",
	module_init,
	module_close
};
