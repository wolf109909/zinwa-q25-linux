/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-modem-helpers-mtk-ccci.h"

static void
test_eif (void)
{
    MMMtkCcciEif eif;

    g_assert_true (mm_mtk_ccci_parse_eif ("\r\n+EIF: 300, ifup, 3, 10.31.4.5, 0\r\n", &eif));
    g_assert_cmpuint (eif.id % 100, ==, 0);
    g_assert_true (eif.up);
    g_assert_cmpstr (eif.address, ==, "10.31.4.5");
    g_assert_true (mm_mtk_ccci_parse_eif ("+EIF: 401,\"ifup\",3,\"100.64.1.2\"", &eif));
    g_assert_cmpuint (eif.id % 100, ==, 1);
    g_assert_true (mm_mtk_ccci_parse_eif ("+EIF: 401, ifdown\r\n", &eif));
    g_assert_false (eif.up);
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 321,ifup,3,10.0.0.1", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 300,ifup,3,999.0.0.1", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 300,ifup,3,0.0.0.0", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 300,ifup,4,2001:db8::1", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 4294967296,ifdown", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: -1,ifdown", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: ,ifdown", &eif));
    g_assert_false (mm_mtk_ccci_parse_eif ("+EIF: 300,ifup", &eif));
}

static void
test_dns (void)
{
    g_auto(GStrv) dns = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *reply =
        "+CGCONTRDP: 2,5,\"ims\",\"10.0.0.2.255.255.255.255\",\"0.0.0.0\",\"1.1.1.1\",\"2.2.2.2\"\r\n"
        "+CGCONTRDP: 1,5,\"ctnet\",\"10.0.0.3.255.255.255.255\",\"0.0.0.0\",\"202.96.128.86\",\"202.96.128.166\"\r\n";

    dns = mm_mtk_ccci_parse_dns (reply, "ctnet", "10.0.0.3", &error);
    g_assert_no_error (error);
    g_assert_cmpstr (dns[0], ==, "202.96.128.86");
    g_assert_cmpstr (dns[1], ==, "202.96.128.166");
    g_assert_null (dns[2]);
    g_clear_pointer (&dns, g_strfreev);
    dns = mm_mtk_ccci_parse_dns ("+CGCONTRDP: 1,5,ctnet,10.0.0.3,0.0.0.0,1.1.1.1,1.1.1.1", "ctnet", "10.0.0.3", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (g_strv_length (dns), ==, 1);
    g_clear_pointer (&dns, g_strfreev);
    dns = mm_mtk_ccci_parse_dns (reply, "ctnet", "10.0.0.9", &error);
    g_assert_null (dns);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);
    dns = mm_mtk_ccci_parse_dns ("+CGCONTRDP: 1,5,ctnet,10.0.0.3,0.0.0.0,0.0.0.0,::", "ctnet", "10.0.0.3", &error);
    g_assert_null (dns);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_dns_no_local_address (void)
{
    const gchar *reply =
        "+CGCONTRDP: 1,5,\"ims\",\"\",\"\",\"1.1.1.1\",\"2.2.2.2\"\r\n"
        "+CGCONTRDP: 0,5,\"ctnet.MNC011.MCC460.GPRS\",\"\",\"\",\"36.14.0.90.0.0.0.0.0.0.0.0.0.0.102.102\",\"\"\r\n"
        "+CGCONTRDP: 0,5,\"ctnet.MNC011.MCC460.GPRS\",\"\",\"\",\"218.2.2.2\",\"218.4.4.4\",\"\",\"\",0,,0,,\r\n";
    g_auto(GStrv) dns = NULL;
    g_autoptr(GError) error = NULL;

    dns = mm_mtk_ccci_parse_dns (reply, "CTNET", "10.0.0.3", &error);
    g_assert_no_error (error);
    g_assert_cmpstr (dns[0], ==, "218.2.2.2");
    g_assert_cmpstr (dns[1], ==, "218.4.4.4");
    g_assert_null (dns[2]);
    g_clear_pointer (&dns, g_strfreev);
    dns = mm_mtk_ccci_parse_dns (reply, "ctwap", "10.0.0.3", &error);
    g_assert_null (dns);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);
    dns = mm_mtk_ccci_parse_dns ("+CGCONTRDP: 0,5,ctnet.other,,,1.1.1.1,2.2.2.2",
                                "ctnet", "10.0.0.3", &error);
    g_assert_null (dns);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_apn (void)
{
    g_assert_true (mm_mtk_ccci_valid_apn ("ctnet"));
    g_assert_true (mm_mtk_ccci_valid_apn ("internet.mnc011.mcc460.gprs"));
    g_assert_false (mm_mtk_ccci_valid_apn (NULL));
    g_assert_false (mm_mtk_ccci_valid_apn (""));
    g_assert_false (mm_mtk_ccci_valid_apn ("evil\"\r\nAT+CFUN=0"));
}

static void
test_cireg (void)
{
    static const struct {
        const gchar *reply;
        gint reporting, registered;
        gboolean known;
        guint32 capabilities;
    } valid[] = {
        { "+CIREG: 0,0", 0, 0, FALSE, 0 },
        { "+CIREG:0,0", 0, 0, FALSE, 0 }, /* Retained stock diagnostic shape. */
        { "+CIREG: 2,0", 2, 0, FALSE, 0 },
        { "+CIREG: 0,1", 0, 1, FALSE, 0 },
        /* Synthetic compatibility fixtures, not decoded r4 handset values. */
        { "+CIREG:0,0,0", 0, 0, FALSE, 0 },
        { "+CIREG:0,1,0", 0, 1, FALSE, 0 },
        { "+CIREG:0,0,5", 0, 0, FALSE, 0 },
        { "+CIREG:2,0,5", 2, 0, FALSE, 0 },
        { "+CIREG:2,0,FFFFFFFF", 2, 0, FALSE, 0 },
        { " \r\n+CIREG: 1 , 0 , 0000000a\r\n", 1, 0, FALSE, 0 },
        { "+CIREG:2,1,0", 2, 1, FALSE, 0 },
        { "+CIREG:2,1,00000000", 2, 1, FALSE, 0 },
        { "+CIREG:0,1,5", 0, 1, TRUE, 5 }, /* Not gated by reporting mode. */
        { "+CIREG: 2,1,5", 2, 1, TRUE, 5 },
        { " \r\n+CIREG: 2 ,\t1 , 0000000a\r\n", 2, 1, TRUE, 10 },
        { "+CIREG: 1,1,FFFFFFFF", 1, 1, TRUE, G_MAXUINT32 },
        { "+CIREG: 2,1,10", 2, 1, TRUE, 16 },
    };
    static const gchar *invalid[] = {
        NULL, "", "OK", "unknown", "+CIREG: 0", "+CIREG: 0,", "+CIREG: ,0",
        "+CIREG: 3,1", "+CIREG: 0,2", "+CIREG: -1,1", "+CIREG: 01,1",
        "+CIREG: 0,4294967296", "+CIREG: 2,1,", "+CIREG:0,0junk", "+CIREG: 2,1,5,0",
        "+CIREG: 3,1,0", "+CIREG: 0,2,0", "+CIREG: 01,0,0", "+CIREG: 0,-1,0",
        "+CIREG: ,0,0", "+CIREG: 0,,0", "+CIREG: 0,0,", "+CIREG: 0,1,",
        "+CIREG: 0,0,\"0\"", "+CIREG: 0,0,0x0", "+CIREG: 0,0,000000000",
        "+CIREG: 0,0,100000000", "+CIREG: 0,0,0junk", "+CIREG: 0,0,+0", "+CIREG: 0,0,-0",
        "+CIREG: 0,0,0,0", "+CIREG: 0,0,0\r\n+CMT: synthetic-private-record",
        "+CIREG: 2,1,0x5", "+CIREG: 2,1,\"5\"",
        "+CIREG: 2,1,100000000", "+CIREG: 2,1,000000005", "+CIREG: 2,1,-5",
        "+CIREG: 2,1,g", "+CIREG: 2,1,5,1", "+CIREG: 2,1,5 tail",
        "AT+CIREG?\r\n+CIREG: 2,1,5", "+CIREGU: 1,5", "+CIREP: 0,0",
        "+CIREG: 2,1,5\r\n+CIREG: 2,1,5", "+CIREG: 2,\n1,5",
        "+CIREG: 2,1,5\r\n+CMT: synthetic-private-record", "+CIREG: 2,\v1,5",
    };
    MMMtkCcciCireg result;
    g_autofree gchar *long_reply = g_strnfill (MM_MTK_CCCI_IMS_RESPONSE_MAX + 1, ' ');
    guint i;

    for (i = 0; i < G_N_ELEMENTS (valid); i++) {
        g_assert_true (mm_mtk_ccci_parse_cireg (valid[i].reply, &result));
        g_assert_cmpint (result.reporting, ==, valid[i].reporting);
        g_assert_cmpint (result.registered, ==, valid[i].registered);
        g_assert_cmpint (result.capabilities_known, ==, valid[i].known);
        g_assert_cmpuint (result.capabilities, ==, valid[i].capabilities);
    }
    for (i = 0; i < G_N_ELEMENTS (invalid); i++) {
        g_assert_false (mm_mtk_ccci_parse_cireg (invalid[i], &result));
        g_assert_cmpint (result.reporting, ==, -1);
        g_assert_cmpint (result.registered, ==, -1);
        g_assert_false (result.capabilities_known);
        g_assert_cmpuint (result.capabilities, ==, 0);
    }
    memcpy (long_reply, "+CIREG:0,0,0", strlen ("+CIREG:0,0,0"));
    g_assert_false (mm_mtk_ccci_parse_cireg (long_reply, &result));
    g_assert_cmpint (result.registered, ==, -1);
    long_reply[MM_MTK_CCCI_IMS_RESPONSE_MAX] = '\0';
    g_assert_true (mm_mtk_ccci_parse_cireg (long_reply, &result));
    g_assert_cmpint (result.registered, ==, 0);
    g_assert_false (result.capabilities_known);
}

static void
test_cirep (void)
{
    static const struct {
        const gchar *reply;
        gint reporting, vops, n3gpp;
    } valid[] = {
        { "+CIREP: 0,0", 0, 0, -1 },
        { "+CIREP: 1,1", 1, 1, -1 },
        { "+CIREP: 1,0,1", 1, 0, 1 },
        { "\r\n+CIREP: 0 ,\t1, 0 \r\n", 0, 1, 0 },
    };
    static const gchar *invalid[] = {
        NULL, "", "ERROR", "unknown", "+CIREP: 0", "+CIREP: 0,", "+CIREP: ,1",
        "+CIREP: 2,1", "+CIREP: 0,2", "+CIREP: 0,1,2", "+CIREP: -1,1",
        "+CIREP: 1,1,", "+CIREP: 1,1,0,1", "+CIREP: 1,1,00", "+CIREP: 1,1,0x1",
        "+CIREP: 1,1,4294967296", "+CIREP: 1,1,\"1\"", "+CIREP: 1,1,1 tail",
        "+CIREP: 1,1\r\nOK", "+CIREP: 1,\n1", "+CIREPI: 1", "+CIREG: 1,1,5",
        "+CIREP: 1,1\r\n+CMT: synthetic-private-record",
    };
    MMMtkCcciCirep result;
    g_autofree gchar *long_reply = g_strnfill (120, '1');
    guint i;

    for (i = 0; i < G_N_ELEMENTS (valid); i++) {
        g_assert_true (mm_mtk_ccci_parse_cirep (valid[i].reply, &result));
        g_assert_cmpint (result.reporting, ==, valid[i].reporting);
        g_assert_cmpint (result.vops, ==, valid[i].vops);
        g_assert_cmpint (result.vops_n3gpp, ==, valid[i].n3gpp);
    }
    for (i = 0; i < G_N_ELEMENTS (invalid); i++) {
        g_assert_false (mm_mtk_ccci_parse_cirep (invalid[i], &result));
        g_assert_cmpint (result.reporting, ==, -1);
        g_assert_cmpint (result.vops, ==, -1);
        g_assert_cmpint (result.vops_n3gpp, ==, -1);
    }
    g_assert_false (mm_mtk_ccci_parse_cirep (long_reply, &result));
}

static void
test_ims_shape (void)
{
    static const struct {
        const gchar *reply, *tag;
        gint prefix, multiline, fields, decimal, hex;
    } cases[] = {
        { "+CIREG:0,0", "+CIREG:", 1, 0, 2, 3, 3 },
        { "\r\n+CIREG: 2 , 1 , aF \r\n", "+CIREG:", 1, 0, 3, 3, 7 },
        /* Shape flags do not identify the actual r4 values or replace enum
         * range/complete-tail checks, including for compatible padding. */
        { "+CIREG:0,0,0", "+CIREG:", 1, 0, 3, 7, 7 },
        { "+CIREG:2,1,5,0,synthetic-private", "+CIREG:", 1, 0, 4, 7, 7 },
        { "+CIREG:0,0junk", "+CIREG:", 1, 0, 2, 1, 1 },
        { "+CIREG:2,1,\"5\"", "+CIREG:", 1, 0, 3, 3, 3 },
        { "+CIREG:2,1,0x5", "+CIREG:", 1, 0, 3, 3, 3 },
        { "+CIREG:2,1,100000000", "+CIREG:", 1, 0, 3, 7, 3 },
        { "+CIREG:2,1,4294967296", "+CIREG:", 1, 0, 3, 3, 3 },
        { "+CIREG:2,1,4294967295", "+CIREG:", 1, 0, 3, 7, 3 },
        { "+CIREG:2,1,", "+CIREG:", 1, 0, 3, 3, 3 },
        { "+CIREG:", "+CIREG:", 1, 0, 0, 0, 0 },
        { "", "+CIREG:", 0, 0, -1, -1, -1 },
        { "synthetic-private", "+CIREG:", 0, 0, -1, -1, -1 },
        { "+CIREGU:0,0", "+CIREG:", 0, 0, -1, -1, -1 },
        { "+CIREG:0,0\r\n+CMT:synthetic-private", "+CIREG:", 1, 1, -1, -1, -1 },
        { "+CIREG:0,0\vsynthetic-private", "+CIREG:", 1, 1, -1, -1, -1 },
        { "+CIREP:1,1", "+CIREP:", 1, 0, 2, 3, 3 },
        { "+CIREP:1,1", "+CIREG:", 0, 0, -1, -1, -1 },
    };
    MMMtkCcciImsShape shape;
    g_autofree gchar *boundary = g_strnfill (MM_MTK_CCCI_IMS_RESPONSE_MAX, 'x');
    g_autofree gchar *long_reply = g_strnfill (4096, 'x');
    guint i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++) {
        mm_mtk_ccci_inspect_ims_response (cases[i].reply, cases[i].tag, &shape);
        g_assert_cmpint (shape.length, ==, (gint) strlen (cases[i].reply));
        g_assert_cmpint (shape.prefix_matches, ==, cases[i].prefix);
        g_assert_cmpint (shape.multiline, ==, cases[i].multiline);
        g_assert_cmpint (shape.fields, ==, cases[i].fields);
        g_assert_cmpint (shape.decimal_fields, ==, cases[i].decimal);
        g_assert_cmpint (shape.hex_fields, ==, cases[i].hex);
    }
    mm_mtk_ccci_inspect_ims_response (boundary, "+CIREG:", &shape);
    g_assert_cmpint (shape.length, ==, 96);
    g_assert_cmpint (shape.prefix_matches, ==, 0);
    mm_mtk_ccci_inspect_ims_response (long_reply, "+CIREG:", &shape);
    g_assert_cmpint (shape.length, ==, 97);
    g_assert_cmpint (shape.prefix_matches, ==, -1);
    g_assert_cmpint (shape.fields, ==, -1);
    g_assert_cmpint (shape.decimal_fields, ==, -1);
    mm_mtk_ccci_inspect_ims_response (NULL, NULL, &shape);
    g_assert_cmpint (shape.length, ==, -1);
    g_assert_cmpint (shape.multiline, ==, -1);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/mtk-ccci/eif", test_eif);
    g_test_add_func ("/mtk-ccci/dns", test_dns);
    g_test_add_func ("/mtk-ccci/apn", test_apn);
    g_test_add_func ("/mtk-ccci/dns-no-local-address", test_dns_no_local_address);
    g_test_add_func ("/mtk-ccci/cireg", test_cireg);
    g_test_add_func ("/mtk-ccci/cirep", test_cirep);
    g_test_add_func ("/mtk-ccci/ims-shape", test_ims_shape);
    return g_test_run ();
}
