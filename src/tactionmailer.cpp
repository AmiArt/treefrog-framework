/* Copyright (c) 2011-2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <QProcess>
#include <TActionMailer>
#include <TActionView>
#include <TAppSettings>
#include <TDispatcher>
#include <TMailMessage>
#include <TSendmailMailer>
#include <TSmtpMailer>
#include <TWebApplication>

constexpr auto CONTROLLER_NAME = "mailer";
constexpr auto ACTIONE_NAME = "mail";

/*!
  \class TActionMailer
  \brief The TActionMailer class provides a mail client on action controller.
*/

/*!
  Constructor.
*/
TActionMailer::TActionMailer() :
    TAbstractController()
{
}

/*!
  Returns the controller name, "mailer".
*/
QString TActionMailer::name() const
{
    return CONTROLLER_NAME;
}

/*!
  Returns a active action name, "mail".
*/
QString TActionMailer::activeAction() const
{
    return ACTIONE_NAME;
}

/*!
  Delivers an email generated by setting variables to the specified template.
*/
bool TActionMailer::deliver(const QString &templateName)
{
    // Creates the view-object
    TDispatcher<TActionView> viewDispatcher(viewClassName(CONTROLLER_NAME, templateName));
    TActionView *view = viewDispatcher.object();
    if (!view) {
        tSystemError("no such template : %s", qPrintable(templateName));
        return false;
    }

    view->setVariantMap(allVariants());
    QString msg = view->toString();
    if (msg.isEmpty()) {
        tSystemError("Mail Message Empty: template name:%s", qPrintable(templateName));
        return false;
    }

    TMailMessage mail(msg, Tf::appSettings()->value(Tf::ActionMailerCharacterSet, "UTF-8").toByteArray());

    // Sets SMTP settings
    bool delay = Tf::appSettings()->value(Tf::ActionMailerDelayedDelivery, false).toBool();

    QByteArray dm = Tf::appSettings()->value(Tf::ActionMailerDeliveryMethod).toByteArray().toLower();
    if (dm == "smtp") {
        // SMTP
        TSmtpMailer *mailer = new TSmtpMailer();
        mailer->setHostName(Tf::appSettings()->value(Tf::ActionMailerSmtpHostName).toByteArray());
        mailer->setPort(Tf::appSettings()->value(Tf::ActionMailerSmtpPort).toUInt());
        mailer->setSslEnabled(Tf::appSettings()->value(Tf::ActionMailerSmtpEnableSSL).toBool());
        mailer->setStartTlsEnabled(Tf::appSettings()->value(Tf::ActionMailerSmtpEnableSTARTTLS).toBool());
        mailer->setAuthenticationEnabled(Tf::appSettings()->value(Tf::ActionMailerSmtpAuthentication).toBool());
        mailer->setUserName(Tf::appSettings()->value(Tf::ActionMailerSmtpUserName).toByteArray());
        mailer->setPassword(Tf::appSettings()->value(Tf::ActionMailerSmtpPassword).toByteArray());
        tSystemDebug("%s", mail.toByteArray().data());

        // POP before SMTP
        if (Tf::appSettings()->value(Tf::ActionMailerSmtpEnablePopBeforeSmtp, false).toBool()) {
            QByteArray popSvr = Tf::appSettings()->value(Tf::ActionMailerSmtpPopServerHostName).toByteArray();
            quint16 popPort = Tf::appSettings()->value(Tf::ActionMailerSmtpPopServerPort).toInt();
            bool apop = Tf::appSettings()->value(Tf::ActionMailerSmtpPopServerEnableApop, false).toBool();

            mailer->setPopBeforeSmtpAuthEnabled(popSvr, popPort, apop, true);
        }

        // Sends email
        if (delay) {
            mailer->moveToThread(Tf::app()->databaseContextMainThread());
            mailer->sendLater(mail);
        } else {
            mailer->send(mail);
            mailer->deleteLater();
        }

    } else if (dm == "sendmail") {
        // Command location of 'sendmail'
        QString cmd = Tf::appSettings()->value(Tf::ActionMailerSendmailCommandLocation).toString().trimmed();
        if (cmd.isEmpty()) {
            cmd = Tf::appSettings()->value(Tf::ActionMailerSendmailCommandLocation).toString().trimmed();
        }

        if (!cmd.isEmpty()) {
            TSendmailMailer *mailer = new TSendmailMailer(cmd);
            QByteArray rawmail = mail.toByteArray();
            QByteArrayList recipients = mail.recipients();

            if (delay) {
                mailer->moveToThread(Tf::app()->databaseContextMainThread());
                mailer->sendLater(mail);
            } else {
                mailer->send(mail);
                mailer->deleteLater();
            }
        }

    } else if (dm.isEmpty()) {
        // not send
    } else {
        // Bad parameter
        tSystemError("SMTP: Bad Parameter: ActionMailer.DeliveryMethod: %s", dm.data());
        return false;
    }
    return true;
}
