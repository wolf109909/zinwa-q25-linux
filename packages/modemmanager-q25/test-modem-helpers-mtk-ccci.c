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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/mtk-ccci/eif", test_eif);
    g_test_add_func ("/mtk-ccci/dns", test_dns);
    g_test_add_func ("/mtk-ccci/apn", test_apn);
    g_test_add_func ("/mtk-ccci/dns-no-local-address", test_dns_no_local_address);
    return g_test_run ();
}
