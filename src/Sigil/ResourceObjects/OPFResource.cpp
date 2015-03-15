/************************************************************************
**
**  Copyright (C) 2013              John Schember <john@nachtimwald.com>
**  Copyright (C) 2009, 2010, 2011  Strahinja Markovic  <strahinja.markovic@gmail.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include <memory>

#include <QtCore/QBuffer>
#include <QtCore/QDate>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <QRegularExpression>

#include "BookManipulation/CleanSource.h"
#include "BookManipulation/XhtmlDoc.h"
#include "Misc/Language.h"
#include "Misc/Utility.h"
#include "ResourceObjects/HTMLResource.h"
#include "ResourceObjects/ImageResource.h"
#include "ResourceObjects/NCXResource.h"
#include "ResourceObjects/OPFResource.h"
#include "ResourceObjects/OPFParser.h"

#include "sigil_constants.h"

static const QString SIGIL_VERSION_META_NAME  = "Sigil version";
static const QString OPF_XML_NAMESPACE        = "http://www.idpf.org/2007/opf";
static const QString FALLBACK_MIMETYPE        = "text/plain";
static const QString ITEM_ELEMENT_TEMPLATE    = "<item id=\"%1\" href=\"%2\" media-type=\"%3\"/>";
static const QString ITEMREF_ELEMENT_TEMPLATE = "<itemref idref=\"%1\"/>";
static const QString OPF_REWRITTEN_COMMENT    = "<!-- Your OPF file was broken so Sigil "
        "tried to rebuild it for you. -->";

static const QString TEMPLATE_TEXT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"BookId\">\n\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
    "    <dc:identifier opf:scheme=\"UUID\" id=\"BookId\">urn:uuid:%1</dc:identifier>\n"
    "  </metadata>\n\n"
    "  <manifest>\n"
    "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
    "  </manifest>\n\n"
    "  <spine toc=\"ncx\">\n"
    "  </spine>\n\n"
    "  <guide>\n\n</guide>\n\n"
    "</package>";


OPFResource::OPFResource(const QString &mainfolder, const QString &fullfilepath, QObject *parent)
  : XMLResource(mainfolder, fullfilepath, parent)
{
    CreateMimetypes();
    FillWithDefaultText();
    // Make sure the file exists on disk.
    // Among many reasons, this also solves the problem
    // with the Book Browser not displaying an icon for this resource.
    SaveToDisk();
}


bool OPFResource::RenameTo(const QString &new_filename)
{
    // The user is not allowed to rename the OPF file.
    return false;
}


Resource::ResourceType OPFResource::Type() const
{
    return Resource::OPFResourceType;
}


QString OPFResource::GetText() const
{
    return TextResource::GetText();
}


void OPFResource::SetText(const QString &text)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(text);
    TextResource::SetText(source);
}


GuideSemantics::GuideSemanticType OPFResource::GetGuideSemanticTypeForResource(const Resource &resource) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    return GetGuideSemanticTypeForResource(resource, p);
}


QString OPFResource::GetGuideSemanticNameForResource(Resource *resource)
{
    return GuideSemantics::Instance().GetGuideName(GetGuideSemanticTypeForResource(*resource));
}

QHash <QString, QString>  OPFResource::GetGuideSemanticNameForPaths()
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QHash <QString, QString> semantic_types;

    foreach(GuideEntry ge, p.m_guide) {
        QString href = ge.m_href;
        QStringList parts = href.split('#', QString::KeepEmptyParts);

        QString type_text = ge.m_type;
        GuideSemantics::GuideSemanticType type =
            GuideSemantics::Instance().MapReferenceTypeToGuideEnum(type_text);
        semantic_types[parts.at(0)] = GuideSemantics::Instance().GetGuideName(type);
    }

    // Cover image semantics don't use reference
    int pos  = GetCoverMeta(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        QString cover_id = me.m_atts.value(QString("content"),QString(""));
        ManifestEntry man = p.m_manifest.at(p.m_idpos[cover_id]);
        QString href = man.m_href;
        GuideSemantics::GuideSemanticType type =
                    GuideSemantics::Instance().MapReferenceTypeToGuideEnum("cover");
        semantic_types[href] = GuideSemantics::Instance().GetGuideName(type);
    }

    return semantic_types;
}

QHash <Resource *, int>  OPFResource::GetReadingOrderAll( const QList <Resource *> resources)
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QHash <Resource *, int> reading_order;
    QHash<QString, int> id_order;
    for (int i = 0; i < p.m_spine.count(); ++i) {
      id_order[p.m_spine.at(i).m_idref] = i;
    }
    QHash<Resource *, QString> id_mapping = GetResourceManifestIDMapping(resources, p);
    foreach(Resource *resource, resources) {
        reading_order[resource] = id_order[id_mapping[resource]];
    }
    return reading_order;
}

int OPFResource::GetReadingOrder(const ::HTMLResource &html_resource) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    const Resource &resource = *static_cast<const Resource *>(&html_resource);
    QString resource_id = GetResourceManifestID(resource, p);
    for (int i = 0; i < p.m_spine.count(); ++i) {
      QString idref = p.m_spine.at(i).m_idref;
      if (resource_id == idref) {
          return i;
      }
    }
    return -1;
}

QString OPFResource::GetMainIdentifierValue() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    int i = GetMainIdentifier(p);
    if (i > -1) {
        return QString(p.m_metadata.at(i).m_content);
    }
    return QString();
}

void OPFResource::SaveToDisk(bool book_wide_save)
{
    QString source = CleanSource::ProcessXML(GetText());
    // Work around for covers appearing on the Nook. Issue 942.
    source = source.replace(QRegularExpression("<meta content=\"([^\"]+)\" name=\"cover\""), "<meta name=\"cover\" content=\"\\1\"");
    TextResource::SetText(source);
    TextResource::SaveToDisk(book_wide_save);
}

QString OPFResource::GetUUIDIdentifierValue()
{
    EnsureUUIDIdentifierPresent();
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if(me.m_name.startsWith("dc:identifier")) {
            QString value = QString(me.m_content).remove("urn:uuid:");
            if (!QUuid(value).isNull()) {
              return value;
            }
        }
    }
    // EnsureUUIDIdentifierPresent should ensure we
    // never reach here.
    Q_ASSERT(false);
    return QString();
}


void OPFResource::EnsureUUIDIdentifierPresent()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if(me.m_name.startsWith("dc:identifier")) {
            QString value = QString(me.m_content).remove("urn:uuid:");
            if (!QUuid(value).isNull()) {
                return;
            }
        }
    }
    QString uuid = Utility::CreateUUID();
    WriteIdentifier("UUID", uuid, p);
    UpdateText(p);
}

QString OPFResource::AddNCXItem(const QString &ncx_path)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString path_to_oebps_folder = QFileInfo(GetFullPath()).absolutePath() + "/";
    QString ncx_oebps_path  = QString(ncx_path).remove(path_to_oebps_folder);
    int n = p.m_manifest.count();
    ManifestEntry me;
    me.m_id = GetUniqueID("ncx", p);
    me.m_href = ncx_oebps_path;
    me.m_mtype = "application/x-dtbncx+xml";
    p.m_manifest.append(me);
    p.m_idpos[me.m_id] = n;
    p.m_hrefpos[me.m_href] = n;
    UpdateText(p);
    return me.m_id;
}


void OPFResource::UpdateNCXOnSpine(const QString &new_ncx_id)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString ncx_id = p.m_spineattr.m_atts.value(QString("toc"),"");
    if (new_ncx_id != ncx_id) {
        p.m_spineattr.m_atts[QString("toc")] = new_ncx_id;
        UpdateText(p);
    }
}


void OPFResource::UpdateNCXLocationInManifest(const ::NCXResource &ncx)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString ncx_id = p.m_spineattr.m_atts.value(QString("toc"),"");
    int pos = p.m_idpos.value(ncx_id, -1);
    if (pos > -1) {
        ManifestEntry me = p.m_manifest.at(pos);
        QString href = me.m_href;
        me.m_href = ncx.Filename();
        p.m_manifest.replace(pos, me);
        p.m_hrefpos.remove(href);
        p.m_hrefpos[ncx.Filename()] = pos;
        UpdateText(p);
    }
}


void OPFResource::AddSigilVersionMeta()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if ((me.m_name == "meta") && (me.m_atts.contains("name"))) {  
            QString name = me.m_atts[QString("name")];
            if (name == SIGIL_VERSION_META_NAME) {
                me.m_atts["content"] = QString(SIGIL_VERSION);
                p.m_metadata.replace(i, me);
                UpdateText(p);
                return;
            }
        }
    }
    MetaEntry me;
    me.m_name = "meta";
    me.m_atts[QString("name")] = QString("Sigil version");
    me.m_atts[QString("content")] = QString(SIGIL_VERSION);
    p.m_metadata.append(me);
    UpdateText(p);
}


bool OPFResource::IsCoverImage(const ::ImageResource &image_resource) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString resource_id = GetResourceManifestID(image_resource, p);
    return IsCoverImageCheck(resource_id, p);
}


bool OPFResource::IsCoverImageCheck(QString resource_id, const OPFParser & p) const
{
    int pos = GetCoverMeta(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        return me.m_atts.value(QString("content"),QString("")) == resource_id;
    }
    return false;
}


bool OPFResource::CoverImageExists() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    return GetCoverMeta(p) > -1;
}


void OPFResource::AutoFixWellFormedErrors()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    SetText(source);
}


QStringList OPFResource::GetSpineOrderFilenames() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QStringList filenames_in_reading_order;
    for (int i=0; i < p.m_spine.count(); ++i) {
        SpineEntry sp = p.m_spine.at(i);
        QString idref = sp.m_idref;
        int pos = p.m_idpos.value(idref,-1);
        if (pos > -1) {
            QString href = p.m_manifest.at(pos).m_href;
            QString filename = QFileInfo(href).fileName();
            filenames_in_reading_order.append(filename);
        }
    }
    return filenames_in_reading_order;
}

#if 0
void OPFResource::SetSpineOrderFromFilenames(const QStringList spineOrder)
{
    QWriteLocker locker(&GetLock());
    std::shared_ptr<xc::DOMDocument> document = GetDocument();
    QList<xc::DOMElement *> items =
        XhtmlDoc::GetTagMatchingDescendants(*document, "item", OPF_XML_NAMESPACE);
    QHash<QString, QString> filename_to_id_mapping;
    foreach(xc::DOMElement * item, items) {
        QString id   = XtoQ(item->getAttribute(QtoX("id")));
        QString href = XtoQ(item->getAttribute(QtoX("href")));
    }
    QList<xc::DOMElement *> itemrefs =
        XhtmlDoc::GetTagMatchingDescendants(*document, "itemref", OPF_XML_NAMESPACE);
    QList<xc::DOMElement *> newSpine;
    foreach(QString spineItem, spineOrder) {
        QString id = filename_to_id_mapping[ spineItem ];
        bool found = false;
        QListIterator<xc::DOMElement *> spineElementSearch(itemrefs);

        while (spineElementSearch.hasNext() && !found) {
            xc::DOMElement *spineElement = spineElementSearch.next();

            if (XtoQ(spineElement->getAttribute(QtoX("idref"))) == spineItem) {
                newSpine.append(spineElement);
                found = true;
            }
        }
    }
    xc::DOMElement *spine = GetSpineElement(*document);
    if (!spine) {
        return;
    }
    XhtmlDoc::RemoveChildren(*spine);
    QListIterator<xc::DOMElement *> spineWriter(newSpine);

    while (spineWriter.hasNext()) {
        spine->appendChild(spineWriter.next());
    }

    UpdateTextFromDom(*document);
}
#endif

QList<Metadata::MetaElement> OPFResource::GetDCMetadata() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QList<Metadata::MetaElement> metadata;
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name.startsWith("dc:")) {
            Metadata::MetaElement book_meta = Metadata::Instance().MapMetaEntryToBookMetadata(me);
            if (!book_meta.name.isEmpty() && !book_meta.value.toString().isEmpty()) {
                metadata.append(book_meta);
            }
        }
    }
    return metadata;
}


QList<QVariant> OPFResource::GetDCMetadataValues(QString text) const
{
    QList<QVariant> values;
    foreach(Metadata::MetaElement meta, GetDCMetadata()) {
        if (meta.name == text) {
            values.append(meta.value);
        }
    }
    return values;
}


void OPFResource::SetDCMetadata(const QList<Metadata::MetaElement> &metadata)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    RemoveDCElements(p);
    foreach(Metadata::MetaElement book_meta, metadata) {
        MetadataDispatcher(book_meta, p);;
    }
    UpdateText(p);
}


void OPFResource::AddResource(const Resource &resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    ManifestEntry me;
    me.m_id = GetUniqueID(GetValidID(resource.Filename()),p);
    me.m_href = resource.GetRelativePathToOEBPS();
    me.m_mtype = GetResourceMimetype(resource);
    int n = p.m_manifest.count();
    p.m_manifest.append(me);
    p.m_idpos[me.m_id] = n;
    p.m_hrefpos[me.m_href] = n;
    if (resource.Type() == Resource::HTMLResourceType) {
        SpineEntry se;
        se.m_idref = me.m_id;
        p.m_spine.append(se);
    }
    UpdateText(p);
}

void OPFResource::RemoveCoverMetaForImage(const Resource &resource, OPFParser& p)
{
    int pos = GetCoverMeta(p);
    QString resource_id = GetResourceManifestID(resource, p);

    // Remove entry if there is a cover in meta and if this file is marked as cover
    if (pos > -1) {
       MetaEntry me = p.m_metadata.at(pos);
       if (me.m_atts.value(QString("content"),QString("")) == resource_id) {
           p.m_metadata.removeAt(pos);
       }
    }
}

void OPFResource::AddCoverMetaForImage(const Resource &resource, OPFParser& p)
{
    int pos = GetCoverMeta(p);
    QString resource_id = GetResourceManifestID(resource, p);

    // If a cover entry exists, update its id, else create one
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        me.m_atts["content"] = resource_id;
        p.m_metadata.replace(pos, me);
    } else {
        MetaEntry me;
        me.m_name = "meta";
        me.m_atts["name"] = "cover";
        me.m_atts["content"] = QString(resource_id);
        p.m_metadata.append(me);
    }
}

void OPFResource::RemoveResource(const Resource &resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    if (p.m_manifest.isEmpty()) return;

    QString resource_oebps_path = resource.GetRelativePathToOEBPS();
    int pos = p.m_hrefpos.value(resource_oebps_path, -1);
    QString item_id = "";

    // Delete the meta tag for cover images before deleting the manifest entry
    if (resource.Type() == Resource::ImageResourceType) {
      RemoveCoverMetaForImage(resource, p);
    }
    if (pos > -1) {
        item_id = p.m_manifest.at(pos).m_id;
    }
    if (resource.Type() == Resource::HTMLResourceType) {
        for (int i=0; i < p.m_spine.count(); ++i) {
            QString idref = p.m_spine.at(i).m_idref;
            if (idref == item_id) {
                p.m_spine.removeAt(i);
                break;
            }
        }
        RemoveGuideReferenceForResource(resource, p);
    }
    if (pos > -1) {
        p.m_manifest.removeAt(pos);
        // rebuild the maps since updating them item by item would be slower
        p.m_idpos.clear();
        p.m_hrefpos.clear();
        for (int i=0; i < p.m_manifest.count(); ++i) {
            p.m_idpos[p.m_manifest.at(i).m_id] = i;
            p.m_hrefpos[p.m_manifest.at(i).m_href] = i;
        }
    }
    UpdateText(p);
}


void OPFResource::AddGuideSemanticType(
    const ::HTMLResource &html_resource,
    GuideSemantics::GuideSemanticType new_type)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    GuideSemantics::GuideSemanticType current_type = GetGuideSemanticTypeForResource(html_resource, p);

    if (current_type != new_type) {
      RemoveDuplicateGuideTypes(new_type, p);
      SetGuideSemanticTypeForResource(new_type, html_resource, p);
    }
    // If the current type is the same as the new one,
    // we toggle it off.
    else {
      RemoveGuideReferenceForResource(html_resource, p);
    }
    UpdateText(p);
}


void OPFResource::SetResourceAsCoverImage(const ::ImageResource &image_resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    if (IsCoverImage(image_resource)) {
        RemoveCoverMetaForImage(image_resource, p);
    } else {
        AddCoverMetaForImage(image_resource, p);
    }
    UpdateText(p);
}


// note: under epub3 spine elements may have page properties set, so simply clearing the
// spine will lose these attributes.  We should try to keep as much of the spine properties
// and linear attributes as we can.  Either that or make the HTML Resource remember its own
// spine page properties, linear attribute, etc

void OPFResource::UpdateSpineOrder(const QList<::HTMLResource *> html_files)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QList<SpineEntry> new_spine;
    foreach(HTMLResource * html_resource, html_files) {
        const Resource &resource = *static_cast<const Resource *>(html_resource);
        QString id = GetResourceManifestID(resource, p);
        int found = -1;
        for (int i = 0; i < p.m_spine.count(); ++i) {
           SpineEntry se = p.m_spine.at(i);
           if (se.m_idref == id) {
               found = i;
               break;
           }
        }
        if (found > -1) {
            new_spine.append(p.m_spine.at(found));
        } else {
            SpineEntry se;
            se.m_idref = id;
            new_spine.append(se);
        }
    }
    p.m_spine.clear();
    p.m_spine = new_spine;
    UpdateText(p);
}


void OPFResource::ResourceRenamed(const Resource &resource, QString old_full_path)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString path_to_oebps_folder = QFileInfo(GetFullPath()).absolutePath() + "/";
    QString resource_oebps_path  = QString(old_full_path).remove(path_to_oebps_folder);
    QString old_id;
    QString new_id;
    for (int i=0; i < p.m_manifest.count(); ++i) {
        QString href = p.m_manifest.at(i).m_href;
        if (href == resource_oebps_path) {
            ManifestEntry me = p.m_manifest.at(i);
            QString old_href = me.m_href;
            me.m_href = resource.GetRelativePathToOEBPS();
            old_id = me.m_id;
            new_id = GetUniqueID(GetValidID(resource.Filename()),p);
            me.m_id = new_id;
            p.m_idpos.remove(old_id);
            p.m_idpos[new_id] = i;
            p.m_hrefpos.remove(old_href);
            p.m_hrefpos[me.m_href] = i;
            p.m_manifest.replace(i, me);
            break;
        }
    }
    for (int i=0; i < p.m_spine.count(); ++i) {
        QString idref = p.m_spine.at(i).m_idref;
        if (idref == old_id) {
            SpineEntry se = p.m_spine.at(i);
            se.m_idref = new_id;
            p.m_spine.replace(i, se);
            break;
        }
    }

    if (resource.Type() == Resource::ImageResourceType) {
        // Change meta entry for cover if necessary
        // Check using IDs since file is already renamed
      if (IsCoverImageCheck(old_id, p)) {
            // Add will automatically replace an existing id
            // Assumes only one cover but removing duplicates
            // can cause timing issues
        AddCoverMetaForImage(resource, p);
        }
    }
    UpdateText(p);
}


int OPFResource::GetGuideReferenceForResourcePos(const Resource &resource, const OPFParser& p) const
{
    QString resource_oebps_path = resource.GetRelativePathToOEBPS();
    for (int i=0; i < p.m_guide.count(); ++i) {
        GuideEntry ge = p.m_guide.at(i);
        QString href = ge.m_href;
        QStringList parts = href.split('#', QString::KeepEmptyParts);
        if (parts.at(0) == resource_oebps_path) {
            return i;
        }
    }
    return -1;
}


void OPFResource::RemoveGuideReferenceForResource(const Resource &resource, OPFParser& p)
{
    if (p.m_guide.isEmpty()) return;
    int pos = GetGuideReferenceForResourcePos(resource, p);
    if (pos > -1) {
        p.m_guide.removeAt(pos);
    }
}


GuideSemantics::GuideSemanticType OPFResource::GetGuideSemanticTypeForResource(const Resource &resource, const OPFParser& p) const
{
  int pos = GetGuideReferenceForResourcePos(resource, p);
    if (pos > -1) {
        GuideEntry ge = p.m_guide.at(pos);
        QString type = ge.m_type;
        return GuideSemantics::Instance().MapReferenceTypeToGuideEnum(type);
    }
    return GuideSemantics::NoType;
}


void OPFResource::SetGuideSemanticTypeForResource(GuideSemantics::GuideSemanticType type,
                                                  const Resource &resource, OPFParser& p)
{
  int pos = GetGuideReferenceForResourcePos(resource, p);
    QString type_attribute;
    QString title_attribute;
    std::tie(type_attribute, title_attribute) = GuideSemantics::Instance().GetGuideTypeMapping()[ type ];

    if (pos > -1) {
        GuideEntry ge = p.m_guide.at(pos);
        ge.m_type = type_attribute;
        ge.m_title = title_attribute;
        p.m_guide.replace(pos, ge);
    } else {
        GuideEntry ge;
        ge.m_type = type_attribute;
        ge.m_title = title_attribute;
        ge.m_href = resource.GetRelativePathToOEBPS();
        p.m_guide.append(ge);
    }
}


void OPFResource::RemoveDuplicateGuideTypes(GuideSemantics::GuideSemanticType new_type, OPFParser& p)
{
    // Industry best practice is to have only one
    // <guide> reference type instance per book.
    // For NoType, there is nothing to remove.
    if (new_type == GuideSemantics::NoType) {
        return;
    }

    if (p.m_guide.isEmpty()) return;

    // build up the list to be deleted in reverse order
    QList<int> dellist;
    for (int i = p.m_guide.count() - 1; i >= 0; --i) {
        GuideEntry ge = p.m_guide.at(i);
        QString type_text = ge.m_type;
        GuideSemantics::GuideSemanticType current_type = GuideSemantics::Instance().MapReferenceTypeToGuideEnum(type_text);
        if (current_type == new_type) {
            dellist.append(i);
        }
    }
    // remove them from the list in reverse order
    foreach(int index, dellist) {
        p.m_guide.removeAt(index);
    }
}


int OPFResource::GetCoverMeta(const OPFParser& p) const
{
    for (int i = 0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if ((me.m_name == "meta") && (me.m_atts.contains(QString("name")))) {
            QString name = me.m_atts[QString("name")];
            if (name == "cover") {
                return i;
            }
        }
    }
    return -1;
}


int OPFResource::GetMainIdentifier(const OPFParser& p) const
{
    QString unique_identifier = p.m_package.m_uniqueid;
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name == "dc:identifier") {
            QString id = me.m_atts.value("id", "");
            if (id == unique_identifier) {
                return i;
            }
        }
    }
    return -1;
}


QString OPFResource::GetResourceManifestID(const Resource &resource, const OPFParser& p) const
{
    QString oebps_path = resource.GetRelativePathToOEBPS();
    int pos = p.m_hrefpos.value(oebps_path,-1);
    if (pos > -1) { 
        return QString(p.m_manifest.at(pos).m_id); 
    }
    return QString();
}


QHash<Resource *, QString> OPFResource::GetResourceManifestIDMapping(const QList<Resource *> resources, 
                                                                     const OPFParser& p)
{
    QHash<Resource *, QString> id_mapping;
    foreach(Resource * resource, resources) {
        QString oebps_path = resource->GetRelativePathToOEBPS();
        int pos = p.m_hrefpos.value(oebps_path,-1);
        if (pos > -1) { 
            id_mapping[ resource ] = p.m_manifest.at(pos).m_id;
        }
    }
    return id_mapping;
}


#if 0
void OPFResource::SetMetaElementsLast(xc::DOMDocument &document)
{
    QList<xc::DOMElement *> metas =
        XhtmlDoc::GetTagMatchingDescendants(document, "meta", OPF_XML_NAMESPACE);
    xc::DOMElement *metadata = GetMetadataElement(document);
    if (!metadata) {
        return;
    }
    foreach(xc::DOMElement * meta, metas) {
        // This makes sure that the <meta> elements come last
        metadata->removeChild(meta);
        metadata->appendChild(meta);
    }
}
#endif

void OPFResource::RemoveDCElements(OPFParser& p)
{
    int pos = GetMainIdentifier(p);
    // build list to be delted in reverse order
    QList<int> dellist;
    int n = p.m_metadata.count();
    for (int i = n-1; i >= 0; --i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name.startsWith("dc:")) {
            if (i != pos) {
               dellist.append(i);
            }
        }
    }
    // delete the MetaEntries in reverse order to not mess up indexes
    foreach(int index, dellist) {
        p.m_metadata.removeAt(index);
    }
}


void OPFResource::MetadataDispatcher(const Metadata::MetaElement &book_meta, OPFParser& p)
{
    // We ignore badly formed meta elements.
    if (book_meta.name.isEmpty() || book_meta.value.isNull()) {
        return;
    }

    // Write Relator codes (always write author as relator code)
    if (Metadata::Instance().IsRelator(book_meta.name) || book_meta.name == "author") {
      WriteCreatorOrContributor(book_meta, p);
    }
    // There is a relator for the publisher, but there is
    // also a special publisher element that we would rather use
    else if (book_meta.name == "pub") {
      WriteSimpleMetadata("publisher", book_meta.value.toString(), p);
    } else if (book_meta.name  == "language") {
        WriteSimpleMetadata(book_meta.name,
                            Language::instance()->GetLanguageCode(book_meta.value.toString()), p);
    } else if (book_meta.name  == "identifier") {
      WriteIdentifier(book_meta.file_as, book_meta.value.toString(), p);
    } else if (book_meta.name == "date") {
      WriteDate(book_meta.file_as, book_meta.value,p);
    } else {
      WriteSimpleMetadata(book_meta.name, book_meta.value.toString(), p);
    }
}


void OPFResource::WriteCreatorOrContributor(const Metadata::MetaElement book_meta, OPFParser& p)
{
    QString value = book_meta.value.toString();
    QString file_as = book_meta.file_as;
    QString role_type = book_meta.role_type;
    QString name = book_meta.name;

    if (name == "author") {
        name = "aut";
    }

    // Must have a role type
    if (role_type.isEmpty()) {
        role_type = "contributor";
    }

    MetaEntry me;
    me.m_name = QString("dc:") + role_type;
    me.m_atts[QString("opf:role")] = name;
    if (!file_as.isEmpty()) {
        me.m_atts[QString("opf:file-as")] = file_as;
    }
    me.m_content = value;
    p.m_metadata.append(me);
}


void OPFResource::WriteSimpleMetadata(const QString &metaname, const QString &metavalue, OPFParser& p)
{
    MetaEntry me;
    me.m_name = QString("dc:") + metaname;
    me.m_content = metavalue;
    p.m_metadata.append(me);
}


void OPFResource::WriteIdentifier(const QString &metaname, const QString &metavalue, OPFParser& p)
{
    int pos = GetMainIdentifier(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        QString scheme = me.m_atts.value(QString("scheme"),QString(""));
        if ((metavalue == me.m_content) && (metaname == scheme)) {
            return;
        }
    }
    MetaEntry me;
    me.m_name = QString("dc:identifier");
    me.m_atts[QString("opf:scheme")] = metaname;
    if (metaname.toLower() == "uuid" && !metavalue.contains("urn:uuid:")) {
        me.m_content = QString("urn:uuid:")  + metavalue;
    } else {
        me.m_content = metavalue;
    }
    p.m_metadata.append(me);
}

void OPFResource::AddModificationDateMeta()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText());
    OPFParser p;
    p.parse(source);
    QString date;
    QDate d = QDate::currentDate();
    // We can't use QDate.toString() because it will take into account the locale. Which mean we may not get Arabic 
    // numerals if the local uses it's own numbering system. So we use this instead to ensure we get a valid date per
    // the epub spec.
    QTextStream(&date) << d.year() << "-" << (d.month() < 10 ? "0" : "") << d.month() << "-" << (d.day() < 10 ? "0" : "") << d.day();
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name == QString("dc:date")) {
            QString etype = me.m_atts.value(QString("opf:event"), QString(""));
            if (etype == QString("modification")) {
                me.m_content = date;
                p.m_metadata.replace(i, me);
                UpdateText(p);
                return;
            }
            
        }
    }
    MetaEntry me;
    me.m_name = QString("dc:date");
    me.m_content = date;
    me.m_atts["xmlns:opf"]="http://www.idpf.org/2007/opf";
    me.m_atts[QString("opf:event")] = QString("modification");
    p.m_metadata.append(me);
    UpdateText(p);
}

void OPFResource::WriteDate(const QString &metaname, const QVariant &metavalue, OPFParser& p)
{
    QString date;
    QDate d = metavalue.toDate();
    // We can't use QDate.toString() because it will take into account the locale. Which mean we may not get Arabic 
    // numerals if the local uses it's own numbering system. So we use this instead to ensure we get a valid date per
    // the epub spec.
    QTextStream(&date) << d.year() << "-" << (d.month() < 10 ? "0" : "") << d.month() << "-" << (d.day() < 10 ? "0" : "") << d.day();

    // This assumes that the "dc" prefix has been declared for the DC namespace
    QHash<QString,QString> atts;
    atts["opf:event"] = metaname;
    MetaEntry me;
    me.m_name=QString("dc:date");
    me.m_content = date;
    me.m_atts[QString("opf:event")] = metaname;
    p.m_metadata.append(me);
}

#if 0
bool OPFResource::BasicStructurePresent(const xc::DOMDocument &document)
{
    QList<xc::DOMElement *> packages =
        XhtmlDoc::GetTagMatchingDescendants(document, "package", OPF_XML_NAMESPACE);

    if (packages.count() != 1) {
        return false;
    }

    QList<xc::DOMElement *> metadatas =
        XhtmlDoc::GetTagMatchingDescendants(document, "metadata", OPF_XML_NAMESPACE);

    if (metadatas.count() != 1) {
        return false;
    }

    QList<xc::DOMElement *> manifests =
        XhtmlDoc::GetTagMatchingDescendants(document, "manifest", OPF_XML_NAMESPACE);

    if (manifests.count() != 1) {
        return false;
    }

    QList<xc::DOMElement *> spines =
        XhtmlDoc::GetTagMatchingDescendants(document, "spine", OPF_XML_NAMESPACE);

    if (spines.count() != 1) {
        return false;
    }

    if (GetMainIdentifierUnsafe() == -1) {
        return false;
    }

    return true;
}

std::shared_ptr<xc::DOMDocument> OPFResource::CreateOPFFromScratch(const xc::DOMDocument *d) const
{
    xc::DOMElement *elem;
    QList<xc::DOMElement *> children;
    QString xml_source;
    QString manifest;
    QString spine;
    QString metadata_content;
    QList<std::pair<QString, QString>> manifest_file;
    QList<std::pair<QString, QString>> manifest_recovered;
    QString manifest_content;
    QList<QString> ids_in_manifest;
    QList<QString> spine_file;
    QList<QString> spine_recovered;
    QString spine_content;
    QString guide_content;
    QString item_id;
    QString path;
    QStringList relative_oebps_paths;
    QString id;
    QString href;
    QString mime;
    bool exists;

    // Try to pull as much as we can out of the originial OPF is present.
    if (d) {
        metadata_content = XhtmlDoc::GetNodeChildrenAsString(GetMetadataElement(*d));
        guide_content = XhtmlDoc::GetNodeChildrenAsString(GetGuideElement(*d));

        elem = GetManifestElement(*d);
        if (elem) {
            children = XhtmlDoc::GetTagMatchingDescendants(*elem, "item");;
            for (int i=0; i<children.length(); ++i) {
                if (XtoQ(children.at(i)->getAttribute(QtoX("media-type"))).toLower() == NCX_MIMETYPE) {
                    continue;
                }
                id = XtoQ(children.at(i)->getAttribute(QtoX("id")));
                href = XtoQ(children.at(i)->getAttribute(QtoX("href")));
                if (!id.isEmpty() && !href.isEmpty()) {
                    manifest_recovered.append(std::pair<QString, QString>(id, href));
                }
            }
        }

        elem = GetSpineElement(*d);
        if (elem) {
            children = XhtmlDoc::GetTagMatchingDescendants(*elem, "itemref");;
            for (int i=0; i<children.length(); ++i) {
                id = XtoQ(children.at(i)->getAttribute(QtoX("idref")));
                if (!id.isEmpty()) {
                    spine_recovered.append(id);
                }
            }
        }
    }

    // Get a list of all items on disk.
    relative_oebps_paths = GetRelativePathsToAllFilesInOEPBS();
    foreach(path, relative_oebps_paths) {
        // The OPF is not allowed to be in the manifest and the NCX
        // is already in the template.
        if (path.contains(OPF_FILE_NAME) || path.contains(NCX_FILE_NAME)) {
            continue;
        }

        item_id = GetValidID(QFileInfo(path).fileName());
        manifest_file.append(std::pair<QString, QString>(item_id, path));

        if (TEXT_EXTENSIONS.contains(QFileInfo(path).suffix().toLower())) {
            spine_file.append(item_id);
        }
    }

    // Compare the recovered with on disk content to ensure we aren't missing anything.
    // Put anything recovered that exists on disk into content.
    for (int i=0; i<manifest_recovered.count(); ++i) {
        std::pair<QString, QString> rec = manifest_recovered.at(i);
        item_id = rec.first;
        path = rec.second;
        for (int j=0; j<manifest_file.count(); ++j) {
            std::pair<QString, QString> frec = manifest_file.at(j);
            if (frec.second == path) {
                mime = GetFileMimetype(path);
                manifest_content.append(QString(ITEM_ELEMENT_TEMPLATE).arg(item_id).arg(path).arg(mime));
                if (mime == "application/xhtml+xml") {
                    ids_in_manifest.append(item_id);
                }
                break;
            }
        }
    }
    // Put anything that exists on disk that isn't in recovered into content.
    for (int i=0; i<manifest_file.count(); ++i) {
        std::pair<QString, QString> frec = manifest_file.at(i);
        exists = false;
        item_id = frec.first;
        path = frec.second;
        for (int j=0; i<manifest_recovered.count(); ++j) {
            std::pair<QString, QString> rec = manifest_recovered.at(j);
            if (rec.second == path) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            mime = GetFileMimetype(path);
            manifest_content.append(QString(ITEM_ELEMENT_TEMPLATE).arg(item_id).arg(path).arg(mime));
            if (mime == "application/xhtml+xml") {
                ids_in_manifest.append(item_id);
            }
        }
    }

    // Compare the recovered sine with the items that are in the manifest. We only
    // use a recovered spine item if the id exists in the manfiest.
    foreach (id, spine_recovered) {
        if (ids_in_manifest.contains(id)) {
            spine_content.append(ITEMREF_ELEMENT_TEMPLATE.arg(id));
        }
    }
    // Add any spine items that are on disk that are not in the recovered spine to the spine.
    foreach (id, spine_file) {
        if (!spine_recovered.contains(id) && ids_in_manifest.contains(id)) {
            spine_content.append(ITEMREF_ELEMENT_TEMPLATE.arg(id));
        }
    }

    // Build the OPF.
    xml_source = GetOPFDefaultText();
    xml_source.replace("</manifest>", manifest_content + "</manifest>")
    .replace("</spine>", spine_content + "</spine>")
    .replace("<metadata", OPF_REWRITTEN_COMMENT + "<metadata")
    .replace("</metadata>", metadata_content + "</metadata>")
    .replace("</guide>", guide_content + "</guide>")
    .replace("<guide>\n\n</guide>\n\n", "");

    std::shared_ptr<xc::DOMDocument> document = XhtmlDoc::LoadTextIntoDocument(xml_source);
    document->setXmlStandalone(true);
    return document;
}
#endif

// Yeah, we could get this list of paths with the GetSortedContentFilesList()
// func from FolderKeeper, but let's not create a strong coupling from
// the opf to the FK just yet. If we can work without that dependency,
// then let's do so.
QStringList OPFResource::GetRelativePathsToAllFilesInOEPBS() const
{
    // The parent folder of the OPF will always be the OEBPS folder.
    QString path_to_oebps_folder = QFileInfo(GetFullPath()).absolutePath();
    QStringList paths = Utility::GetAbsolutePathsToFolderDescendantFiles(path_to_oebps_folder);
    paths.replaceInStrings(Utility::URLEncodePath(path_to_oebps_folder) + "/", "");
    paths.sort();
    return paths;
}


QString OPFResource::GetOPFDefaultText()
{
    return TEMPLATE_TEXT.arg(Utility::CreateUUID());
}


void OPFResource::FillWithDefaultText()
{
    SetText(GetOPFDefaultText());
}


QString OPFResource::GetUniqueID(const QString &preferred_id, const OPFParser& p) const
{
    if (p.m_idpos.contains(preferred_id)) {
        return Utility::CreateUUID();
    }
    return preferred_id;
}


QString OPFResource::GetResourceMimetype(const Resource &resource) const
{
    return GetFileMimetype(resource.Filename());
}


QString OPFResource::GetFileMimetype(const QString &filepath) const
{
    return m_Mimetypes.value(QFileInfo(filepath).suffix().toLower(), FALLBACK_MIMETYPE);
}


// Initializes m_Mimetypes
void OPFResource::CreateMimetypes()
{
    m_Mimetypes[ "jpg"   ] = "image/jpeg";
    m_Mimetypes[ "jpeg"  ] = "image/jpeg";
    m_Mimetypes[ "png"   ] = "image/png";
    m_Mimetypes[ "gif"   ] = "image/gif";
    m_Mimetypes[ "tif"   ] = "image/tiff";
    m_Mimetypes[ "tiff"  ] = "image/tiff";
    m_Mimetypes[ "bm"    ] = "image/bmp";
    m_Mimetypes[ "bmp"   ] = "image/bmp";
    m_Mimetypes[ "svg"   ] = "image/svg+xml";
    m_Mimetypes[ "ncx"   ] = NCX_MIMETYPE;
    // We convert all HTML document types to XHTML
    m_Mimetypes[ "xml"   ] = "application/xhtml+xml";
    m_Mimetypes[ "xhtml" ] = "application/xhtml+xml";
    m_Mimetypes[ "html"  ] = "application/xhtml+xml";
    m_Mimetypes[ "htm"   ] = "application/xhtml+xml";
    m_Mimetypes[ "css"   ] = "text/css";
    // Until the standards gods grace us with font mimetypes,
    // these will have to do
    m_Mimetypes[ "otf"   ] = "application/vnd.ms-opentype";
    m_Mimetypes[ "ttf"   ] = "application/x-font-ttf";
    m_Mimetypes[ "ttc"   ] = "application/x-font-truetype-collection";
}


QString OPFResource::GetRelativePathToRoot() const
{
    QFileInfo info(GetFullPath());
    QDir parent_dir = info.dir();
    QString parent_name = parent_dir.dirName();
    return parent_name + "/" + Filename();
}


void OPFResource::UpdateText(const OPFParser &p)
{
    TextResource::SetText(p.convert_to_xml());
}

