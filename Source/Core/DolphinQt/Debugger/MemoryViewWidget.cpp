// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/MemoryViewWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QtGlobal>

#include <cmath>

#include "Common/Align.h"
#include "Common/FloatUtils.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

// "Most mouse types work in steps of 15 degrees, in which case the delta value is a multiple of
// 120; i.e., 120 units * 1/8 = 15 degrees." (http://doc.qt.io/qt-5/qwheelevent.html#angleDelta)
constexpr double SCROLL_FRACTION_DEGREES = 15.;

MemoryViewWidget::MemoryViewWidget(QWidget* parent) : QTableWidget(parent)
{
  horizontalHeader()->hide();
  verticalHeader()->hide();
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setShowGrid(false);

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(&Settings::Instance(), &Settings::DebugFontChanged, this, &MemoryViewWidget::UpdateFont);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this] { Update(); });
  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &MemoryViewWidget::Update);
  connect(this, &MemoryViewWidget::customContextMenuRequested, this,
          &MemoryViewWidget::OnContextMenu);
  connect(&Settings::Instance(), &Settings::ThemeChanged, this, &MemoryViewWidget::Update);

  // Also calls update.
  UpdateFont();
}

void MemoryViewWidget::UpdateFont()
{
  const QFontMetrics fm(Settings::Instance().GetDebugFont());
  m_font_vspace = fm.lineSpacing();
  // BoundingRect is too unpredictable, a custom one would be needed for each view type. Different
  // fonts have wildly different spacing between two characters and horizontalAdvance includes
  // spacing.
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
  m_font_width = fm.horizontalAdvance(QLatin1Char('0'));
#else
  m_font_width = fm.width(QLatin1Char('0'));
#endif
  setFont(Settings::Instance().GetDebugFont());
  Update();
}

constexpr int GetTypeSize(MemoryViewWidget::Type type)
{
  switch (type)
  {
  case MemoryViewWidget::Type::ASCII:
  case MemoryViewWidget::Type::Hex8:
  case MemoryViewWidget::Type::Unsigned8:
  case MemoryViewWidget::Type::Signed8:
    return 1;
  case MemoryViewWidget::Type::Unsigned16:
  case MemoryViewWidget::Type::Signed16:
  case MemoryViewWidget::Type::Hex16:
    return 2;
  case MemoryViewWidget::Type::Hex32:
  case MemoryViewWidget::Type::Unsigned32:
  case MemoryViewWidget::Type::Signed32:
  case MemoryViewWidget::Type::Float32:
    return 4;
  case MemoryViewWidget::Type::Double:
  case MemoryViewWidget::Type::Hex64:
    return 8;
  default:
    return 1;
  }
}

constexpr int GetCharacterCount(MemoryViewWidget::Type type)
{
  // Max number of characters +1 for spacing between columns.
  switch (type)
  {
  case MemoryViewWidget::Type::ASCII:  // A
    return 2;
  case MemoryViewWidget::Type::Hex8:  // Byte = FF
    return 3;
  case MemoryViewWidget::Type::Unsigned8:  // UCHAR_MAX = 255
    return 4;
  case MemoryViewWidget::Type::Hex16:    // 2 Bytes = FFFF
  case MemoryViewWidget::Type::Signed8:  // CHAR_MIN = -128
    return 5;
  case MemoryViewWidget::Type::Unsigned16:  // USHORT_MAX = 65535
    return 6;
  case MemoryViewWidget::Type::Signed16:  // SHORT_MIN = -32768
    return 7;
  case MemoryViewWidget::Type::Hex32:  // 4 Bytes = FFFFFFFF
    return 9;
  case MemoryViewWidget::Type::Float32:     // Rounded and Negative FLT_MAX = -3.403e+38
  case MemoryViewWidget::Type::Unsigned32:  // UINT_MAX = 4294967295
    return 11;
  case MemoryViewWidget::Type::Double:    // Rounded and Negative DBL_MAX = -1.798e+308
  case MemoryViewWidget::Type::Signed32:  // INT_MIN = -2147483648
    return 12;
  case MemoryViewWidget::Type::Hex64:  // For dual_view + Double. 8 Bytes = FFFFFFFFFFFFFFFF
    return 17;
  default:
    return 10;
  }
}

void MemoryViewWidget::Update()
{
  clearSelection();

  u32 address = m_address;
  address = Common::AlignDown(address, m_alignment);

  const int data_columns = m_bytes_per_row / GetTypeSize(m_type);

  if (m_dual_view)
    setColumnCount(2 + 2 * data_columns);
  else
    setColumnCount(2 + data_columns);

  if (rowCount() == 0)
    setRowCount(1);

  // This sets all row heights and determines horizontal ascii spacing.
  verticalHeader()->setDefaultSectionSize(m_font_vspace - 1);
  verticalHeader()->setMinimumSectionSize(m_font_vspace - 1);
  horizontalHeader()->setMinimumSectionSize(m_font_width * 2);

  const AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(m_address_space);

  // Calculate (roughly) how many rows will fit in our table
  int rows = std::round((height() / static_cast<float>(rowHeight(0))) - 0.25);

  setRowCount(rows);

  for (int i = 0; i < rows; i++)
  {
    u32 row_address = address - ((rowCount() / 2) * m_bytes_per_row) + i * m_bytes_per_row;

    auto* bp_item = new QTableWidgetItem;
    bp_item->setFlags(Qt::ItemIsEnabled);
    bp_item->setData(Qt::UserRole, row_address);

    setItem(i, 0, bp_item);

    auto* row_item =
        new QTableWidgetItem(QStringLiteral("%1").arg(row_address, 8, 16, QLatin1Char('0')));

    row_item->setData(Qt::UserRole, row_address);
    row_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    setItem(i, 1, row_item);

    if (row_address == address)
      row_item->setSelected(true);

    if (Core::GetState() != Core::State::Paused || !accessors->IsValidAddress(row_address))
    {
      for (int c = 2; c < columnCount(); c++)
      {
        auto* item = new QTableWidgetItem(QStringLiteral("-"));
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(Qt::UserRole, row_address);

        setItem(i, c, item);
      }

      continue;
    }
  }

  int starting_column = 2;

  if (m_dual_view)
  {
    // Match left columns to number of right columns.
    Type left_type = Type::Hex32;
    if (GetTypeSize(m_type) == 1)
      left_type = Type::Hex8;
    else if (GetTypeSize(m_type) == 2)
      left_type = Type::Hex16;
    else if (GetTypeSize(m_type) == 8)
      left_type = Type::Hex64;

    UpdateColumns(left_type, starting_column);

    const int column_count = m_bytes_per_row / GetTypeSize(left_type);

    // Update column width
    for (int i = starting_column; i < starting_column + column_count - 1; i++)
      setColumnWidth(i, m_font_width * GetCharacterCount(left_type));

    // Extra spacing between dual views.
    setColumnWidth(starting_column + column_count - 1,
                   m_font_width * (GetCharacterCount(left_type) + 2));

    starting_column += column_count;
  }

  UpdateColumns(m_type, starting_column);
  UpdateBreakpointTags();

  setColumnWidth(0, rowHeight(0));

  for (int i = starting_column; i <= columnCount(); i++)
    setColumnWidth(i, m_font_width * GetCharacterCount(m_type));

  viewport()->update();
  update();
}

void MemoryViewWidget::UpdateColumns(Type type, int first_column)
{
  if (Core::GetState() != Core::State::Paused)
    return;

  const int data_columns = m_bytes_per_row / GetTypeSize(type);
  const AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(m_address_space);

  auto text_alignment = Qt::AlignLeft;
  if (type == Type::Signed32 || type == Type::Unsigned32 || type == Type::Signed16 ||
      type == Type::Unsigned16 || type == Type::Signed8 || type == Type::Unsigned8)
  {
    text_alignment = Qt::AlignRight;
  }

  for (int i = 0; i < rowCount(); i++)
  {
    u32 row_address = item(i, 1)->data(Qt::UserRole).toUInt();
    if (!accessors->IsValidAddress(row_address))
      continue;

    auto update_values = [&](auto value_to_string) {
      for (int c = 0; c < data_columns; c++)
      {
        auto* cell_item = new QTableWidgetItem;
        cell_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        cell_item->setTextAlignment(text_alignment);

        const u32 cell_address = row_address + c * GetTypeSize(type);

        setItem(i, first_column + c, cell_item);

        if (accessors->IsValidAddress(cell_address))
        {
          cell_item->setText(value_to_string(cell_address));
          cell_item->setData(Qt::UserRole, cell_address);
        }
        else
        {
          cell_item->setFlags({});
          cell_item->setText(QStringLiteral("-"));
        }
      }
    };
    switch (type)
    {
    case Type::Hex8:
      update_values([&accessors](u32 address) {
        const u8 value = accessors->ReadU8(address);
        return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0'));
      });
      break;
    case Type::ASCII:
      update_values([&accessors](u32 address) {
        const char value = accessors->ReadU8(address);
        return IsPrintableCharacter(value) ? QString{QChar::fromLatin1(value)} :
                                             QString{QChar::fromLatin1('.')};
      });
      break;
    case Type::Hex16:
      update_values([&accessors](u32 address) {
        const u16 value = accessors->ReadU16(address);
        return QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0'));
      });
      break;
    case Type::Hex32:
      update_values([&accessors](u32 address) {
        const u32 value = accessors->ReadU32(address);
        return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
      });
      break;
    case Type::Hex64:
      update_values([&accessors](u32 address) {
        const u64 value = accessors->ReadU64(address);
        return QStringLiteral("%1").arg(value, 16, 16, QLatin1Char('0'));
      });
      break;
    case Type::Unsigned8:
      update_values(
          [&accessors](u32 address) { return QString::number(accessors->ReadU8(address)); });
      break;
    case Type::Unsigned16:
      update_values(
          [&accessors](u32 address) { return QString::number(accessors->ReadU16(address)); });
      break;
    case Type::Unsigned32:
      update_values(
          [&accessors](u32 address) { return QString::number(accessors->ReadU32(address)); });
      break;
    case Type::Signed8:
      update_values([&accessors](u32 address) {
        return QString::number(Common::BitCast<s8>(accessors->ReadU8(address)));
      });
      break;
    case Type::Signed16:
      update_values([&accessors](u32 address) {
        return QString::number(Common::BitCast<s16>(accessors->ReadU16(address)));
      });
      break;
    case Type::Signed32:
      update_values([&accessors](u32 address) {
        return QString::number(Common::BitCast<s32>(accessors->ReadU32(address)));
      });
      break;
    case Type::Float32:
      update_values([&accessors](u32 address) {
        QString string = QString::number(accessors->ReadF32(address), 'g', 4);
        // Align to first digit.
        if (!string.startsWith(QLatin1Char('-')))
          string.prepend(QLatin1Char(' '));

        return string;
      });
      break;
    case Type::Double:
      update_values([&accessors](u32 address) {
        QString string =
            QString::number(Common::BitCast<double>(accessors->ReadU64(address)), 'g', 4);
        // Align to first digit.
        if (!string.startsWith(QLatin1Char('-')))
          string.prepend(QLatin1Char(' '));

        return string;
      });
      break;
    }
  }
}

void MemoryViewWidget::UpdateBreakpointTags()
{
  if (Core::GetState() != Core::State::Paused)
    return;

  for (int i = 0; i < rowCount(); i++)
  {
    bool row_breakpoint = false;

    for (int c = 2; c < columnCount(); c++)
    {
      // Pull address from cell itself, helpful for dual column view.
      auto cell = item(i, c);
      u32 address = cell->data(Qt::UserRole).toUInt();

      if (address == 0)
      {
        row_breakpoint = false;
        continue;
      }

      // In dual view the only sizes that dont match up on both left and right views are for
      // Double, which uses two or four columns of hex32.
      if (m_address_space == AddressSpace::Type::Effective &&
          PowerPC::memchecks.GetMemCheck(address, GetTypeSize(m_type)) != nullptr)
      {
        row_breakpoint = true;
        cell->setBackground(Qt::red);
      }
    }

    if (row_breakpoint)
    {
      item(i, 0)->setData(Qt::DecorationRole,
                          Resources::GetScaledThemeIcon("debugger_breakpoint")
                              .pixmap(QSize(rowHeight(0) - 3, rowHeight(0) - 3)));
    }
  }
}

void MemoryViewWidget::SetAddressSpace(AddressSpace::Type address_space)
{
  if (m_address_space == address_space)
  {
    return;
  }

  m_address_space = address_space;
  Update();
}

AddressSpace::Type MemoryViewWidget::GetAddressSpace() const
{
  return m_address_space;
}
void MemoryViewWidget::SetDisplay(Type type, int bytes_per_row, int alignment, bool dual_view)

{
  m_type = type;
  m_bytes_per_row = bytes_per_row;
  m_dual_view = dual_view;
  if (alignment == 0)
    m_alignment = GetTypeSize(type);
  else
    m_alignment = alignment;

  Update();
}

void MemoryViewWidget::SetBPType(BPType type)
{
  m_bp_type = type;
}

void MemoryViewWidget::SetAddress(u32 address)
{
  if (m_address == address)
    return;

  m_address = address;

  Update();
}

void MemoryViewWidget::SetBPLoggingEnabled(bool enabled)
{
  m_do_log = enabled;
}

void MemoryViewWidget::resizeEvent(QResizeEvent*)
{
  Update();
}

void MemoryViewWidget::keyPressEvent(QKeyEvent* event)
{
  switch (event->key())
  {
  case Qt::Key_Up:
    m_address -= 16;
    Update();
    return;
  case Qt::Key_Down:
    m_address += 16;
    Update();
    return;
  case Qt::Key_PageUp:
    m_address -= rowCount() * 16;
    Update();
    return;
  case Qt::Key_PageDown:
    m_address += rowCount() * 16;
    Update();
    return;
  default:
    QWidget::keyPressEvent(event);
    break;
  }
}

u32 MemoryViewWidget::GetContextAddress() const
{
  return m_context_address;
}

void MemoryViewWidget::ToggleRowBreakpoint(bool row)
{
  if (m_address_space != AddressSpace::Type::Effective)
    return;

  const u32 addr = row ? m_base_address : GetContextAddress();
  const auto length = GetTypeSize(m_type);
  const int breaks = row ? (m_bytes_per_row / length) : 1;
  bool overlap = false;

  // Row breakpoint should either remove any breakpoint left on the row, or activate all
  // breakpoints.
  if (row && PowerPC::memchecks.OverlapsMemcheck(addr, m_bytes_per_row))
    overlap = true;

  for (int i = 0; i < breaks; i++)
  {
    u32 address = addr + length * i;
    TMemCheck* check_ptr = PowerPC::memchecks.GetMemCheck(address, length);

    if (check_ptr == nullptr && !overlap)
    {
      TMemCheck check;
      check.start_address = address;
      check.end_address = check.start_address + length - 1;
      check.is_ranged = length > 0;
      check.is_break_on_read = (m_bp_type == BPType::ReadOnly || m_bp_type == BPType::ReadWrite);
      check.is_break_on_write = (m_bp_type == BPType::WriteOnly || m_bp_type == BPType::ReadWrite);
      check.log_on_hit = m_do_log;
      check.break_on_hit = true;

      PowerPC::memchecks.Add(check);
    }
    else if (check_ptr != nullptr)
    {
      // Using the pointer fixes misaligned breakpoints (0x11 breakpoint in 0x10 aligned view).
      PowerPC::memchecks.Remove(check_ptr->start_address);
    }
  }

  emit BreakpointsChanged();
  Update();
}

void MemoryViewWidget::ToggleBreakpoint()
{
  ToggleRowBreakpoint(false);
}

void MemoryViewWidget::wheelEvent(QWheelEvent* event)
{
  auto delta =
      -static_cast<int>(std::round((event->angleDelta().y() / (SCROLL_FRACTION_DEGREES * 8))));

  if (delta == 0)
    return;

  m_address += delta * 16;
  Update();
}

void MemoryViewWidget::mousePressEvent(QMouseEvent* event)
{
  auto* item_selected = itemAt(event->pos());
  if (item_selected == nullptr)
    return;

  const u32 addr = item_selected->data(Qt::UserRole).toUInt();

  m_context_address = addr;
  m_base_address = item(row(item_selected), 1)->data(Qt::UserRole).toUInt();

  switch (event->button())
  {
  case Qt::LeftButton:
    if (column(item_selected) == 0)
      ToggleRowBreakpoint(true);
    else
      SetAddress(m_base_address);

    Update();
    break;
  default:
    break;
  }
}

void MemoryViewWidget::OnCopyAddress()
{
  u32 addr = GetContextAddress();
  QApplication::clipboard()->setText(QStringLiteral("%1").arg(addr, 8, 16, QLatin1Char('0')));
}

void MemoryViewWidget::OnCopyHex()
{
  u32 addr = GetContextAddress();

  const auto length = GetTypeSize(m_type);

  const AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(m_address_space);
  u64 value = accessors->ReadU64(addr);

  QApplication::clipboard()->setText(
      QStringLiteral("%1").arg(value, sizeof(u64) * 2, 16, QLatin1Char('0')).left(length * 2));
}

void MemoryViewWidget::OnContextMenu()
{
  auto* menu = new QMenu(this);

  menu->addAction(tr("Copy Address"), this, &MemoryViewWidget::OnCopyAddress);

  auto* copy_hex = menu->addAction(tr("Copy Hex"), this, &MemoryViewWidget::OnCopyHex);

  const AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(m_address_space);
  copy_hex->setEnabled(Core::GetState() != Core::State::Uninitialized &&
                       accessors->IsValidAddress(GetContextAddress()));

  menu->addSeparator();

  menu->addAction(tr("Show in code"), this, [this] { emit ShowCode(GetContextAddress()); });

  menu->addSeparator();

  menu->addAction(tr("Add to watch"), this, [this] {
    const u32 address = GetContextAddress();
    const QString name = QStringLiteral("mem_%1").arg(address, 8, 16, QLatin1Char('0'));
    emit RequestWatch(name, address);
  });
  menu->addAction(tr("Toggle Breakpoint"), this, &MemoryViewWidget::ToggleBreakpoint);

  menu->exec(QCursor::pos());
}
