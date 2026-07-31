/**
 * @file ixmsg.c  Commande CLI pour envoyer un FarEndMessage (canal IX Cisco)
 *
 * Usage: /ixsend <texte>
 *
 * Envoie <texte> comme FarEndMessage (type "Text") sur le canal IX Cisco
 * du premier appel actif trouvé parmi les comptes enregistrés. Nécessite
 * que le canal IX ait été négocié et établi (ix_cisco_enabled=yes dans la
 * config, et handshake UDT terminé côté pair).
 */
#include <re.h>
#include <baresip.h>


static struct call *find_active_call(void)
{
	struct le *le;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;
		struct call *call = ua_call(ua);

		if (call)
			return call;
	}

	return NULL;
}


static int cmd_ixsend(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct call *call;
	struct ix_cisco *ix;
	int err;

	if (!str_isset(carg->prm))
		return re_hprintf(pf, "usage: /ixsend <texte>\n");

	call = find_active_call();
	if (!call)
		return re_hprintf(pf, "ixsend: aucun appel actif\n");

	ix = call_ix(call);
	if (!ix)
		return re_hprintf(pf,
			"ixsend: canal IX non actif sur cet appel "
			"(ix_cisco_enabled=yes ?)\n");

	err = ix_cisco_send_message(ix, carg->prm, "Text");
	if (err)
		return re_hprintf(pf, "ixsend: échec de l'envoi (%m)\n", err);

	return re_hprintf(pf, "FarEndMessage envoyé: \"%s\"\n", carg->prm);
}


static const struct cmd ixmsgcmdv[] = {
{"ixsend", 0, CMD_PRM, "Envoyer un FarEndMessage (canal IX Cisco)", cmd_ixsend},
};


static int module_init(void)
{
	return cmd_register(baresip_commands(),
			     ixmsgcmdv, RE_ARRAY_SIZE(ixmsgcmdv));
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), ixmsgcmdv);
	return 0;
}


const struct mod_export DECL_EXPORTS(ixmsg) = {
	"ixmsg",
	"application",
	module_init,
	module_close
};
